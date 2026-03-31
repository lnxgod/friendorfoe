# WiFi Probe Request Tracking — Interface Contracts

## New Detection Source
- Source string: `"wifi_probe_request"` (value 5 on ESP32)
- Drone ID format: `"probe_AA:BB:CC:DD:EE:FF"` (MAC of probing device)

## ESP32 → Backend (POST /detections/drones)
New fields in detection item:
```json
{
  "source": "wifi_probe_request",
  "drone_id": "probe_AA:BB:CC:DD:EE:FF",
  "ssid": "DJI-Phantom4-ABCDEF",
  "bssid": "AA:BB:CC:DD:EE:FF",
  "rssi": -65,
  "confidence": 0.35,
  "probed_ssids": ["DJI-Phantom4-ABCDEF", "TELLO-AB1234"]
}
```

## Backend → Dashboard/Android
- Classification: "likely_drone" if probed SSID matches drone patterns, else "wifi_device"
- New endpoint: `GET /detections/probes` returns:
```json
{
  "count": 5,
  "devices": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "probed_ssids": ["DJI-Phantom4-ABCDEF", "TELLO-AB1234"],
      "probe_count": 42,
      "best_rssi": -55,
      "classification": "likely_drone",
      "sensor_count": 3,
      "sensors": ["uplink_1", "uplink_2", "uplink_3"],
      "last_seen": 1711734045.2,
      "age_s": 3.2,
      "lat": 37.302870,
      "lon": -120.575395
    }
  ]
}
```

## Key Rules
- Probe request frame: SSID starts at byte 24 (NO 12-byte fixed params like beacons)
- Rate limit: 1 detection per MAC+SSID pair per 5 seconds
- Confidence: 0.50 if probed SSID matches drone pattern, 0.15 if generic
- Feed into triangulation: same as beacon detections (RSSI → distance → EKF)
