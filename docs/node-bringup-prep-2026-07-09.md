# Node Bring-Up Prep - 2026-07-09

Prepared for the next hardware session: bring sensor nodes back online, verify
the badge update path, and then test the new privacy beacon release.

## Current Baseline

Checked at `2026-07-09 08:55:18 PDT`.

- Local backend is running at `http://127.0.0.1:8000` and
  `http://fof-server.local:8000`.
- `/health` reports backend version `0.64.65-privacy-beacons`.
- Redis is currently `unavailable`; database is `ok`.
- `/detections/nodes/status` currently reports `0` online nodes.
- `/detections/diagnostics` reports:
  - `no_uplink_batches_received`
  - `0` online geometry nodes
  - expected production firmware `0.64.65-privacy-beacons`
  - expected badge firmware `0.64.65-badge-privacy-beacons`
  - `scanner_update_blocked_count: 0` because no live scanner heartbeats exist yet
- Scanner readiness is heartbeat-driven; it returns no rows until nodes post.

## Known Registered Nodes

The backend registry still has these real fleet entries:

| Alias/name | Device ID | Notes |
| --- | --- | --- |
| `pool` | `uplink_CC59FC` | real fleet alias in `scripts/fof_flash.py` |
| `area51` | `uplink_24DBB4` | real fleet alias |
| `chomper` | `uplink_CB77A4` | real fleet alias |
| `frontyard` | `uplink_CC558C` | real fleet alias |
| `patio` | `uplink_D0A6AC` | real fleet alias |
| `spare` | `uplink_9AB838` | real fleet alias |
| `lamb` | `uplink_E3A56C` | real fleet alias |
| `gate` | `uplink_D0A148` | canary helper default; geometry excluded |
| `FrontYard` | `uplink_03BBD8` | older duplicate/stale registry row |

The registry also contains smoke/test rows:
`uplink_COORD01`, `uplink_LEGACY01`, `uplink_SMOKE01`, `uplink_TIME01`.
Ignore those unless intentionally testing fixtures.

## Target Firmware Metadata

Backend firmware endpoints are already serving these targets:

| Firmware | Version | Size | SHA-256 |
| --- | --- | ---: | --- |
| `scanner-s3-combo` | `0.64.65-privacy-beacons` | `1135120` | `f6f706ec771fc29b5782c78435fe08dbad02f073b66fdbf3c8fe693bf77235ec` |
| `scanner-s3-combo-seed` | `0.64.65-privacy-beacons` | `1135120` | `cbba607ba12d92eef0a4fb19d279e37fd697fcf2d6c30dbc819c4f2c7fb0bf0b` |
| `uplink-s3` | `0.64.65-privacy-beacons` | `1181024` | `58d500ff1bc34dc04dffecfad8293dbfcfebb21368b9d105d5b919077c892be8` |
| `scanner-s3-combo-fof_badge` | `0.64.65-badge-privacy-beacons` | `1149648` | `be8b00ff57f7912cc2ae41379f4c6c10be509d8fa711a7526af3caf08c482e94` |
| `uplink-s3-fof_badge` | `0.64.65-badge-privacy-beacons` | `1437152` | `52209d3a69284b8d73e2fe6ff06954a19f89bd23fa5293950c837c68988f0032` |

## First Checks When Hardware Is Powered

Run these before flashing anything:

```bash
curl -sS http://127.0.0.1:8000/health
curl -sS http://127.0.0.1:8000/detections/nodes/status | jq .
curl -sS http://127.0.0.1:8000/detections/diagnostics | jq '.firmware_readiness, .ingest_freshness'
```

Once a node appears, check scanner update readiness:

```bash
curl -sS 'http://127.0.0.1:8000/nodes/firmware/scanner/readiness?uart=both' | jq .
curl -sS 'http://127.0.0.1:8000/nodes/firmware/scanner/readiness?device_id=uplink_D0A148&uart=both' | jq .
```

Readiness states to care about:

- `verified`: scanner already reports the target version.
- `pending`: safe candidate for staged update.
- `blocked` with `scanner_command_ingress_unreachable`: do not force fleet rollout;
  that scanner likely needs one USB recovery flash first.
- `missing_uplink_ip`: node heartbeat is not usable yet; fix node network first.

## Recommended Bring-Up Order

1. Confirm backend is still on `0.64.65-privacy-beacons`.
2. Power one node at a time and wait for it in `/detections/nodes/status`.
3. Confirm the node is posting to `http://fof-server.local:8000/detections/drones`.
4. Check `firmware_readiness` and scanner readiness before any flash.
5. Use `gate` as the first canary if it comes online cleanly.
6. Only continue to fleet after one canary scanner verifies via heartbeat version.

## Scanner Update Commands

Dry/readiness canary for gate:

```bash
python3 scripts/gate_canary_flash.py --backend http://127.0.0.1:8000 --node gate --uart both
```

Execute gate canary only after readiness looks sane:

```bash
python3 scripts/gate_canary_flash.py --backend http://127.0.0.1:8000 --node gate --uart both --execute
```

Manual per-node scanner relay helper:

```bash
python3 scripts/fof_flash.py pool --scanner s3-combo --uart ble --backend http://127.0.0.1:8000
python3 scripts/fof_flash.py pool --scanner s3-combo --uart wifi --backend http://127.0.0.1:8000
```

Backend staged scanner rollout path:

```bash
curl -sS -X POST 'http://127.0.0.1:8000/nodes/firmware/scanner/rollout?mode=canary&canary_device_id=uplink_D0A148&canary_uart=both' | jq .
curl -sS 'http://127.0.0.1:8000/nodes/firmware/rollouts/<rollout_id>' | jq .
```

Do not start fleet rollout until a canary target is heartbeat-verified at the
new version. The backend enforces this by default for fleet rollout.

## Badge Update Commands

Badge helper supports USB, AP, and LAN transports:

```bash
python3 scripts/fof_badge_flash.py --dry-run --transport lan --host <badge-ip> --only all
python3 scripts/fof_badge_flash.py --transport lan --host <badge-ip> --only all
```

USB path when a badge is physically attached:

```bash
python3 scripts/fof_badge_flash.py --dry-run --transport usb --port <serial-port> --only all
python3 scripts/fof_badge_flash.py --transport usb --port <serial-port> --only all
```

Use `--only uplink`, `--only scanners`, `--only ble`, or `--only wifi` for
targeted recovery. Use `--skip-current` when the goal is to avoid rewriting
scanner slots that already report `0.64.65-badge-privacy-beacons`.

## Backend Restart If Needed

The live backend was launched from `backend/` as:

```bash
.venv/bin/python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
```

If it drifts or is stopped:

```bash
cd backend
nohup .venv/bin/python -m uvicorn app.main:app --host 0.0.0.0 --port 8000 \
  > /private/tmp/fof-backend-8000.log \
  2> /private/tmp/fof-backend-8000.err \
  < /dev/null &
```

Then verify:

```bash
curl -sS http://127.0.0.1:8000/health
curl -sS http://fof-server.local:8000/health
```

## Stop Rules

- Do not run fleet scanner rollout with zero online nodes.
- Do not run fleet scanner rollout if readiness shows command-ingress blockers.
- Do not assume a release asset means the field update path works; require
  live backend metadata and heartbeat version proof.
- Do not move or reuse a pushed failed tag; roll forward to a new version.
- If a badge or scanner vanishes during update, inspect `/nodes/firmware/operations`
  or the rollout endpoint before trying a different transport.
