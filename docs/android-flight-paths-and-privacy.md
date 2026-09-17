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

## Verification

- 1,096 JVM tests and Android lint passed.
- 35 API 35 emulator tests passed, including the database upgrade, storage
  retention/deletion, timeline controls, expired privacy evidence, and existing
  privacy/detail/navigation content.
- Flight-path and privacy-evidence screenshots were inspected on the emulator.
- Physical Bluetooth/RF behavior still depends on the phone, permissions,
  connected scanner hardware, and the surrounding radio environment.
