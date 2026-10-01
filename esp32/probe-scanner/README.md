# USB Wi-Fi Probe Scanner

Standalone receive-only ESP32-S3 firmware for the Android Wi-Fi probes screen.
Uses the native USB/OTG connector (GPIO19 D− / GPIO20 D+), at least 4 MB flash,
and no PSRAM. Tested by compilation and protocol fixtures; physical phone/board
validation is pending. Do not install on a chip mounted inside a FoF Badge.

## Flash and connect

1. Open https://lnxgod.github.io/friendorfoe/#usb-probe in desktop Chrome or Edge.
2. Plug the board's native USB port into the computer. Hold BOOT, tap RESET,
   release BOOT, then choose **Flash USB Wi-Fi Probe Scanner**.
3. Reset after flashing. Connect that same port to an Android USB-host-capable
   phone using a data cable/OTG adapter. A charging-only cable cannot work.
4. Install Friend or Foe 0.67.25 or newer. Open **More → Wi-Fi probes → USB scanner**
   and allow USB access. The screen connects automatically to one flashed scanner.
5. Keep that screen open for live reports and optional foreground alerts.
   Leaving/backgrounding releases USB and clears the phone's session buffer.

No backend, Wi-Fi credentials, extra board, or Internet connection is needed
while scanning. The S3 listens on channels 1–13, one at a time, dwelling 250 ms.
It does not transmit probe requests, join networks, deauthenticate, or save frames.
It cannot hear 5/6 GHz, and signal strength does not establish physical distance.
A wildcard means any network; binary SSIDs are shown as name not captured.
MAC addresses can rotate and are not reliable person/device identities.

## Build

`pio run -d esp32/probe-scanner -e probe-scanner-s3`

Flash bootloader at `0x0`, partitions at `0x8000`, app at `0x10000`.
The layout is deliberately independent of combo-scanner and badge OTA partitions.
Re-enter the ROM loader using BOOT/RESET before reflashing: this firmware presents
TinyUSB CDC (`303a:4001`, product `FoF WiFi Probe`), not Serial/JTAG.
On two-port boards a CP210x/CH34x UART connector is not the app data path.
If a phone cannot supply sufficient power, use a powered USB OTG hub.

## USB protocol v1

UTF-8 newline-delimited JSON, maximum 512 bytes per line, CDC 115200/8N1,
DTR asserted. Only metadata is sent. Example messages:

```json
{"type":"hello","protocol":1,"boot":"0123456789abcdef","firmware":"fof-wifi-probe","version":"1.0.0","sensor_id":"usb-aabbccddeeff","uptime_ms":1000,"channel":1,"dropped":0}
{"type":"probe","protocol":1,"boot":"0123456789abcdef","seq":1,"uptime_ms":1100,"mac":"02:11:22:33:44:55","ssid":"Home, Wi-Fi","wildcard":false,"binary":false,"rssi":-48,"channel":6}
```

A heartbeat is emitted every two seconds. Android requires the firmware identity
and protocol before accepting reports, rejects duplicate/out-of-order sequences,
clears on reboot/disconnect, and fails after eight seconds without a heartbeat.
Radio callbacks use a nonblocking 64-item queue; a global 20 reports/second cap
bounds work. `dropped` counts budget/queue/USB limits since boot. Phone history is
bounded to 6,000 reports and displays the 500 most recently heard addresses in the
last five minutes. This is sampled activity, not an exhaustive packet capture.
