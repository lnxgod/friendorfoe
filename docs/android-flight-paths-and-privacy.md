# Android flight paths and recent privacy evidence

Available in Android `0.67.19-android-flight-paths` (version code 127).

## Aircraft flight paths

Open an aircraft's details from List or AR and tap **Recorded flight path**.
The same action is available for an aircraft selected on Map and from an
aircraft's saved History entry. Selecting an aircraft on Map also draws its
recorded trail there.

Choose **15 min**, **1 hour**, or **24 hours**. Drag the timeline to inspect a
received position's timestamp, altitude, and speed. **Fit path** restores the
map framing after panning or zooming.

Only positions received while aircraft detection is running are recorded.
There is no historical-provider backfill. A lone saved sighting cannot
reconstruct an older flight. Gaps longer than two minutes, implausible jumps,
and crossings of the map's date-line boundary are drawn as separate segments.
The displayed distance counts only connected observed segments.

Positions are stored on this phone, sampled no more frequently than every five
seconds, and retained for up to 24 hours. Storage is capped at 100,000 positions
overall; the viewer loads at most 3,600 recent positions per aircraft. Map tiles
still require network access unless already cached. Aircraft details refresh
with the live feed and mark departed detections as last known.

Clearing History also deletes stored paths. Deleting the last History entry for
an aircraft deletes its path. Active detection can subsequently record new
positions. Updating the app preserves existing History and stored positions.

## Privacy evidence

On Privacy, **Needs attention** keeps awareness and critical findings while
hiding owned devices. **Live only** limits results to current observations.
These controls filter the display; they do not change detector policy.

Open **Recent encounters** to review evidence after a current finding expires.
Each encounter retains its exact source identity, observed timestamps, update
count, and up to 60 received signal samples. Evidence details can be copied to
the clipboard. Signal trends describe radio strength, not physical distance,
identity, or intent; saved evidence does not mean a device is still nearby.

The encounter log stays in memory for this app session, expires entries after
30 minutes without a new observation, and holds at most 200 encounters. It is
not written to disk and adds no location history. Ignored findings are removed.
**Clear** removes the log; only new observations may appear afterward.

## Beacon grouping and phone detections (0.67.27)

### Expandable device tree (0.67.28)

Android `0.67.28-android-device-tree` (version code 136) adds two collapsed
sections to Privacy: **Trackers** and **Venue beacons**. Open a section, then a
family, then an individual observation:

- Trackers → AirTags & Find My, Find Hub, Tile, Samsung SmartTag, Chipolo,
  or Other trackers.
- Venue beacons → iBeacon, Eddystone, or Other beacons.

Only families currently present are shown. Grouping uses the existing reported
category, title, and evidence; it is not a new detector. The Find My family can
include accessories other than AirTags. Counts describe source observations,
not unique physical devices: sources and rotating broadcast identities remain
separate. Each leaf shows its source, observed ID, freshness, and signal, with
the same Details, Ignore, and supported Track actions.

**Expand all** and **Collapse all** control the full tree. Search and filters
automatically open matching branches. Clearing filters returns to the compact
tree; normal scan updates and screen recreation preserve branch choices.
Awareness and critical findings stay above the tree in their existing sections,
including tracker findings with stronger evidence. This changes presentation,
not alert eligibility, detection thresholds, or radio collection.

Tree screenshots: [collapsed](design/device-tree/collapsed.png),
[family branches](design/device-tree/families.png), and
[beacon observation](design/device-tree/observation.png).

### Original grouping and added detections

Android `0.67.27-android-nearby-detections` (version code 135) collapses routine
venue beacons, including iBeacon and Eddystone, into one group at the bottom of
Privacy. The header counts these separately. Tap the group to inspect individual
observations; search, source/category filters, freshness, Ignore, Details, and
supported RSSI sweeps still work. Separate tracker and attack detections stay outside the
group. Venue-beacon observations are informational across all sources, regardless
of proximity scores, and do not alert.

Enable **Phone privacy scan** and **Wi-Fi anomaly detection** in Settings for
these additions; neither needs USB hardware or a backend:

- **Find Hub accessories:** matches the FEAA service's 0x40/0x41 frames with
  validated lengths, separate from Eddystone. Both 160-bit and 256-bit identifier
  formats are parsed; reception of extended advertisements depends on phone
  support. A nearby accessory may be a tag or headphones. Its advertising mode
  does not prove ownership, separation from an owner, or following. Addresses
  remain separate; the app does not resolve encrypted identifiers or flags.
- **Weak Wi-Fi security:** reports APs advertising WEP or TKIP, including mixed
  TKIP/CCMP support, with the observed network, BSSID, RSSI, and frequency. This
  reports advertised options, not a client's negotiated encryption or an attack.
- **Fewer Wi-Fi false alerts:** WPA3/SAE, OWE, enterprise, DPP, and unknown
  capabilities are not assumed open. A name shared by open and authenticated APs
  is an awareness finding, because legitimate configurations can do this too.

Android's Wi-Fi scan permissions, location-services requirements, and scan
throttling still apply. Raw Wi-Fi probe reception remains the USB scanner's job.
Physical radio reception has not been validated for these additions; parser,
classification, and emulator tests use controlled fixtures.

References: [Google Find Hub frame specification](https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn),
[Android scan capability classification](https://android.googlesource.com/platform/packages/modules/Wifi/+/refs/heads/main/framework/java/android/net/wifi/util/ScanResultUtil.java),
[Android Wi-Fi scanning](https://developer.android.com/develop/connectivity/wifi/wifi-scan),
[WEP/TKIP guidance](https://support.apple.com/en-us/102766).

UI test fixtures: [collapsed](design/beacon-grouping/collapsed.png) and
[expanded](design/beacon-grouping/expanded.png), captured at 1080 × 1920 / 420 dpi.

## Original flight-path release verification

- 1,096 JVM tests and Android lint passed.
- 35 API 35 emulator tests passed, including the database upgrade, storage
  retention/deletion, timeline controls, expired privacy evidence, and existing
  privacy/detail/navigation content.
- Flight-path and privacy-evidence screenshots were inspected on the emulator.
- Physical Bluetooth/RF behavior still depends on the phone, permissions,
  connected scanner hardware, and the surrounding radio environment.

## Nearby aircraft groups (0.67.29)

Android `0.67.29-android-nearby-groups` (version code 137) removes category and
camera-focus boosts from the Nearby list. Distance always determines row order;
unknown, negative, and non-finite distances follow known distances. Stable IDs
break ties so confidence changes do not shuffle equally distant rows.

**By type** groups aircraft inside **Within 10 mi**, **Farther away**, and
**Distance unknown**. Type groups are ordered by their closest member, and
members are ordered nearest first. Helicopters stay together even when their
reported category is government or military; agency labels remain on each row.
Tap section or type headers to collapse or expand them. The distance sections
also remain in **Nearest first**, which lists individual rows without type groups.

Farther-away sections start collapsed. Search and filters open matching branches;
clearing filters restores the compact distance sections. Live updates preserve
manual expansion, including across screen recreation. The header reports nearby,
farther, and unknown-distance counts separately.

**Range** retains the adjustable 1–50 mile setting and 10-mile default. It controls
the nearby section and aircraft notification eligibility, and changes apply to
existing observations immediately. It does not enable notifications. Radio drone
notification policy is unchanged. Selecting a wider range never boosts a
helicopter ahead of a closer aircraft.

New installs and existing priority-mode installs use By type. An explicitly saved
Nearest first choice remains a flat view. Both views use the same distance ordering.

Screenshots: [nearby groups](design/nearby-aircraft/nearby-groups.png) and
[expanded distant helicopters](design/nearby-aircraft/farther-helicopters.png).
