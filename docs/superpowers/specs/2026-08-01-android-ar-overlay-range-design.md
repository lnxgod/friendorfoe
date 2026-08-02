# Android AR Overlay Range Fix

## Goal

Make ADS-B aircraft reliably appear in the Android augmented-reality view without changing firmware or expanding into unrelated app work.

## Behavior

- Aircraft may appear in the AR camera overlay at a maximum slant distance of 12 statute miles (19,312 meters).
- In-view aircraft labels and off-screen aircraft direction arrows use the same 12-mile limit.
- The existing 2-kilometer drone visual range remains unchanged.
- Precise and approximate Android location grants both permit positional AR overlays. Approximate location may produce less accurate placement, so the existing prompt to enable precise location remains visible.
- With location denied or no usable location fix, camera-only visual detection remains available, while geographic aircraft placement remains unavailable.
- Existing safety filters remain: grounded, very-low-altitude, and stale aircraft are not rendered.
- ADS-B acquisition remains enabled by default and continues fetching within its existing 50-nautical-mile search radius.

## Implementation

Centralize aircraft AR range in the position-mapping policy and reuse that policy for off-screen arrow eligibility, avoiding two independent numeric cutoffs. Pass a usable-location decision to the AR screen so both precise and approximate grants display positional overlays.

The adjacent-path audit is limited to obvious conditions that can suppress otherwise valid ADS-B aircraft. It does not redesign AR, change backend APIs, or touch badge firmware.

## Verification

- A focused mapper test proves an aircraft just inside 12 statute miles can be in view and one just outside cannot.
- A focused permission/presentation test proves approximate location does not blank computed overlays.
- Existing focused AR mapper and permission tests are run once after implementation.
- One final debug APK build confirms Android compilation and packaging.

## Release

After verification, publish the fix as the next Android patch release with a signed APK attached to GitHub Releases.
