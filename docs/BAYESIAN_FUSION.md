# Bayesian Sensor Fusion

A drone detection in Friend or Foe is the posterior probability that a
nearby device is a drone, given everything we've heard from it across
sources, decayed by recency. The math is simple, deliberately so, and
runs identically on the scanner (C) and the Android app (Kotlin) so each
stack can serve as a check on the other.

## Log-Odds Combination

We track each device as a log-odds value

```
L = log( P(drone | evidence) / P(¬drone | evidence) )
```

starting at the prior `log(0.1 / 0.9) ≈ −2.197` (we expect ~10 % of
nearby RF devices to be drones absent any evidence —
`PRIOR_PROBABILITY = 0.1` in `esp32/shared/constants.h:15`).

Every observation contributes a likelihood ratio `Λ` for "this evidence
under the drone hypothesis vs. under the not-drone hypothesis." Bayes'
rule in log-odds space is just addition:

```
L_after = L_before + log(Λ)
```

This is why log-odds is the natural representation: independent evidence
chains multiply in probability space and add in log space.

## Per-Source Likelihood Ratios

| Source | Λ | Why |
|---|---|---|
| `LR_BLE_RID` | 50 | FAA Remote ID frame, formal protocol — strong evidence |
| `LR_WIFI_BEACON` (NAN) | 50 | Same as above for WiFi NAN-based RID |
| `LR_WIFI_DJI_IE` | 30 | Vendor-specific DJI IE in beacon, very few non-DJI sources |
| `LR_WIFI_OUI` | 5 | Curated drone-OUI table; covers 29 manufacturers but misses rebadged hardware |
| `LR_WIFI_SSID` | 3 | Pattern matches like "DJI-…", "Tello-…"; consumer cameras can collide |
| `LR_BLE_FINGERPRINT` | 1.6 | Weak — it's a known device class, not specifically a drone |
| `LR_WIFI_ASSOC` | 1.4 | Even weaker — STA → AP traffic, mostly non-drone |

ADS-B aircraft live on the same scale but use `LR_ADSB = 100` because
ADS-B carries an ICAO type code that explicitly identifies the airframe.

Source: `esp32/shared/constants.h:20-27` (drone-specific) and
`backend/app/services/classifier.py` (ADS-B, mobile hotspot, tracker).
The Android `BayesianFusionEngine` in
`android/app/src/main/java/com/friendorfoe/detection/BayesianFusionEngine.kt`
uses the same constants.

## Time Decay

Evidence ages out. Between observations the log-odds decay toward zero
(toward the prior, in probability space) on a 30-second half-life:

```
L_decayed = L * exp(-ln(2) * elapsed / HALF_LIFE)
```

`EVIDENCE_HALF_LIFE_SEC = 30.0` (`constants.h:16`). After 60 s without a
fresh frame, log-odds drop to a quarter of their peak; after 5 minutes,
< 0.1 %. This is what makes the fusion responsive: a drone that flies
out of range fades from the operator's view in seconds rather than
sticking around as a stale "confirmed drone."

The decay code is the first few lines of `bayesian_fusion.c::update_drone`
(line 113).

## Clamping

Log-odds are clamped to `[-7, +7]`, equivalent to probability bounds of
`[≈0.0009, ≈0.9991]`. This prevents one extreme observation (or repeated
echoes of the same beacon) from saturating to certainty. It also keeps
the math numerically stable in single-precision float.

`MAX_LOG_ODDS = 7.0`, `MIN_LOG_ODDS = -7.0` (`constants.h:17-18`).

## Worked Example

A DJI Phantom drone flying past:

| Frame | Source | Λ | log Λ | L |
|---|---|---|---:|---:|
| (prior) | — | — | — | −2.20 |
| t=0 | DJI IE in beacon | 30 | +3.40 | +1.20 |
| t=1 | BLE Remote ID | 50 | +3.91 | +5.11 |
| t=3 | WiFi SSID `DJI-Phantom4-…` | 3 | +1.10 | +6.21 |
| t=5 | DJI IE again | 30 | +3.40 | clamp → +7.00 |

The drone reaches "confirmed" classification well before the third frame.
Compare to a single SSID match without supporting evidence: prior + log(3)
= −1.10, posterior probability ≈ 25 % — surfaces as `possible_drone`,
not `confirmed_drone`. This is why the dashboard's classification is
not derivable from any single source.

## Why Not Maximum Confidence?

A naive design would use `max(confidence per source)` and stop there. We
don't, because the entire point of multi-source detection is that the
union of weak signals is stronger than the strongest individual signal.
A drone broadcasting both BLE Remote ID and WiFi DJI IE is a much higher
posterior than a drone broadcasting only one — Bayesian addition captures
this; max-confidence does not.

## Cross-Stack Parity

The Kotlin and C implementations of this math are byte-equivalent on the
same input — same constants, same decay formula, same clamp. We ship
native tests that verify them both:

- ESP32: `esp32/test/test_bayesian.c` (run via `pio test -e test`)
- Android: `BayesianFusionEngineTest.kt` (run via `./gradlew test`)

If a CFP reviewer wants to double-check the math, both test suites are
expected to pass on the same fixture inputs. That is the parity we mean
when the README says "byte-for-byte parsing across stacks."
