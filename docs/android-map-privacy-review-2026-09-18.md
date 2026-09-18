# Map trails and privacy review — September 18, 2026

Release: 0.67.21-android-map-trails (Android version code 129).

## Aircraft map

Previously, the main map only drew the selected aircraft's trail. Dismissing
its details removed that path. The new independent historical layer defaults
to 15 minutes, with Off, 1 hour, and 24 hours options. Fit trails frames the
recorded paths. Trails remain when an aircraft leaves the live feed, and an
endpoint opens the existing Flight path review screen. Live markers remain
above history; history is never inserted into the live aircraft list.

The map uses the existing local position database. It queries at most 12,000
recent points and displays up to 40 aircraft, newest observation first. It
preserves the existing gap, implausible jump, and dateline segmentation. Search,
category, source, type, distance, and altitude filters apply to trails. Departed
aircraft use their latest recorded endpoint and current viewer position for
range filtering; unknown range is excluded when a distance limit is active.
Only received positions are available, with up to 24 hours retained. No provider
historical backfill or continuous recording while the app is closed is added.
Clearing aircraft history also removes its recorded positions.

## Privacy evidence

Recent encounters now support search, Hide owned (on by default), and Repeated.
Two minutes without reports starts a new observation period. Repeated requires
at least two distinct updates within each of at least two periods. Duplicate,
stale, future, and cached observations cannot manufacture repeated evidence.
A new filter does not change severity or issue a following alert: scanning
interruptions can create gaps, and repeated advertisements do not establish
movement, identity, ownership, or intent.

Period evidence is kept only in the bounded in-memory encounter log. Periods
expire after 30 minutes without a report in that period. Exact source identities,
ignored-device removal, clear behavior, and stable list ordering remain intact.
No new permissions, location collection, background service, or upload is added.

## Verification

- Full 1,110 JVM tests pass, including new projection/filter, gap threshold,
  duplicate report, ownership, search, and period-expiry regressions.
- 17 API 35 emulator tests pass, covering the SQLite history query, independent
  map layer ownership, gap segmentation, endpoint routing, window/retry controls,
  privacy filter reset, flight-path review, and stable native map hosting.
- Android lint passes; debug and instrumented APKs build successfully.
- Emulator visual review uses synthetic DEMO aircraft paths and privacy records.
  Physical radio detection and real-world flight reception require field testing.

## Screenshots

Synthetic emulator data: [main map](screenshots/android-map-privacy/map-trails.png)
and [repeated privacy encounters](screenshots/android-map-privacy/privacy-repeated.png).

## Further improvements worth a separate pass

- Offer an explicit retention setting for aircraft history and a clear explanation
  of existing locally stored phone coordinates and optional network location use.
- Add a user-controlled, redacted evidence export that previews identifiers and
  coordinates before sharing. Current Copy evidence is explicit but includes
  the exact device record identifier.
- Validate long-running scans on real devices with screen-off, Bluetooth changes,
  and Android battery restrictions before adding background encounter guarantees.
