# Backend Port Notes

## Pin and isolation

All evidence is pinned to `VENDOR_BASE`
`2cca5ad8df17ebd8d5f48dc72051441e30df1b8f`. The files beneath
`vendor/` are immutable evidence and are never build inputs.
`tools/vendor_snapshot.py --check` verifies the base file, donor bytes,
declared local snapshot, exact SHA-256 map, and absence of undeclared regular
files without writing. Repository symlink protections apply in materialize and
check modes.

Badge-named evidence, including
`vendor/shared_reference/uart_protocol.h`,
`vendor/shared_reference/badge_threat_policy.[ch]`, calibration/easter-egg
references, and badge-con references inside runtime donors, is provenance only.
It is not imported by the backend runtime. The backend protocol header has no
badge include, message, pin selection, or target-variant branch.

## Backend adaptations

The detection and investigation sinks fail closed with no consumer. Emission is
synchronous: each sink copies its input to a stack snapshot and dispatches that
copy. It never retains a caller pointer. The UART consumer registered by a later
task owns bounded queueing and cross-task lifetime.

Native-linked portable sources are Task-1 identity, detection policy, privacy
signatures, both sinks, the pure glasses classifier, the typed BLE/Wi-Fi feature
adapter, Bayesian fusion, BLE fingerprinting and behavioral threat detection,
DJI/French/OpenDroneID/Wi-Fi Beacon RID/OUI/SSID parsers, and the test clock.
Native parser logging resolves to no-op stubs; timing resolves to the explicitly
settable test clock. The native link includes `-lm`.

`ble_remote_id.c`, `wifi_scanner.c`, the runtime half of
`ble_investigator.c`, and `ble_ja3.c` remain device-only. Their local copies
use backend task priorities and synchronous sinks. The BLE runtime checks the
device-only NVS setting before invoking the pure glasses classifier. Both
pairing-spam and serial-skimmer results enter the detection sink when
`FOF_BACKEND_FIRMWARE=1`. NimBLE, ESP-Wi-Fi, NVS, FreeRTOS, and ESP-IDF JSON
are not native-linked.

## Parity coverage

`test_ported_detectors` carries the permitted pinned donor functions and their
fixture helpers from the six Task-2 donor test blobs. It intentionally omits
the textual `ble_remote_id.c` inclusion and all badge Easter/display cases.
Additional fixtures cover French DRI and Wi-Fi Beacon RID identity, position,
altitude, source, and RSSI. The backend matrix starts with raw BLE
advertisements, real behavioral-detector signals, and source-specific Wi-Fi
observation structs. The production feature adapter constructs and emits the
BLE fingerprint/privacy, Meta, tracker, venue, pairing-spam, serial-skimmer,
Wi-Fi AP, probe, association, anomaly, and lock-on snapshots through the sink.

## Donor ledger

| Donor path | Pinned SHA-256 | Local build/adaptation | Dependency/replacement note |
|---|---|---|---|
| `esp32/scanner/main/detection/bayesian_fusion.c` | `dc05dd1e2871c11b2f4cd2d10a34cba218e8b8a91cb4c46e9d29c1e09cc90369` | `scanner/main/detection/bayesian_fusion.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/bayesian_fusion.h` | `c600cd536f32122604b33b1660044f082b946921be0c02dd66d12d3d072b26b5` | `scanner/main/detection/bayesian_fusion.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/ble_fingerprint.c` | `45eb146fa93230680e211c7e942b2386333743257641475523513414bd0975f9` | `scanner/main/detection/ble_fingerprint.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/ble_fingerprint.h` | `8f7d1e85e3251682a05b6781fe7a2227897d222f3eba5e1f50415521cf171a25` | `scanner/main/detection/ble_fingerprint.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/ble_investigator.c` | `5e311c8ee26f94fc4f7d8a5bbe48997268640887b0c58c1972cbc97e22d9c14a` | `scanner/main/detection/ble_investigator.c` | Pure core plus device runtime; investigation chunks dispatch through backend sink. |
| `esp32/scanner/main/detection/ble_investigator.h` | `53ce42349c76d2ea8ee635881e457fbfb8a6652ccf1ff2a9de9423fde7cd6cce` | `scanner/main/detection/ble_investigator.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/ble_ja3.c` | `1b8eff48c66350884fb8e6eee81c0795cccd08bc2f6b1ed875a8a9a88d47df3c` | `scanner/main/detection/ble_ja3.c` | Verbatim device-only NimBLE structural classifier. |
| `esp32/scanner/main/detection/ble_ja3.h` | `0d3ff26c4557922ebdfb83d650f98ee3516a3074d272cf9d156c123ce7c271be` | `scanner/main/detection/ble_ja3.h` | Verbatim device-only NimBLE structural classifier. |
| `esp32/scanner/main/detection/ble_remote_id.c` | `506a59f334a1e0177a735e3e523bca6763928c11341c4555bb998374eba32b72` | `scanner/main/detection/ble_remote_id.c` | Device-only: backend priorities/settings/classifier/sink; calibration, Easter egg, badge-con, and direct UART removed. |
| `esp32/scanner/main/detection/ble_remote_id.h` | `4640a0a9bd7c1bd46eb99b1d8bd1d38969f29bec8324b097dc21b2ad0ad4e372` | `scanner/main/detection/ble_remote_id.h` | Device-only API no longer accepts/retains caller queues. |
| `esp32/scanner/main/detection/ble_threat_detector.c` | `c353e19f998b952ed66b31ab678c81021dc6c82e39828ad2845ba6b2f0a7abf1` | `scanner/main/detection/ble_threat_detector.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/ble_threat_detector.h` | `5e4514cc40c36695e6abd173dd7c203d677a47133bffb2dc3d1cedf12dea9e14` | `scanner/main/detection/ble_threat_detector.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/dji_drone_id_parser.c` | `9bfb829fef0bfe1ecec0f7b50b1e5a796a3b52c9b7dbfd79155b3e5fe521ffa0` | `scanner/main/detection/dji_drone_id_parser.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/dji_drone_id_parser.h` | `94792af5e8287271344f0aaa593eb4a5d84db8da8a4e201f2d2682b2ebea19a6` | `scanner/main/detection/dji_drone_id_parser.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/french_dri_parser.c` | `d81dd5b15aa61c3d9af264a0c7895bdac9a725ff12b6c090a53276354a98d760` | `scanner/main/detection/french_dri_parser.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/french_dri_parser.h` | `848bed75517f3e6cbe63fdd08ea592dc4225db20673f9d84b232af1cf119df9a` | `scanner/main/detection/french_dri_parser.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/glasses_detector.c` | `65915f4ea6494d5bd023d17d50078c19a544f0ac04e3374bbea46f7d1aa33761` | `scanner/main/detection/backend_glasses_classifier.c`; `backend_glasses_settings.c` | Split into pure observed-time classifier and device-only NVS settings. |
| `esp32/scanner/main/detection/glasses_detector.h` | `852a55d9cdc9224fa2c6095a21d6dd703afa5748ff377cb5a62cbaa37a0a9417` | `scanner/main/detection/backend_glasses_classifier.h`; `backend_glasses_settings.h` | Split into pure observed-time classifier and device-only NVS settings. |
| `esp32/scanner/main/detection/open_drone_id_parser.c` | `4fff8237ea2396f57d0bca94de752a297672dcd89fb4db62e06179cd21f8ae8f` | `scanner/main/detection/open_drone_id_parser.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/open_drone_id_parser.h` | `bb13b1095455c729ed410250a43e4f271889948a89fe2322c4ab416af7fe4d2e` | `scanner/main/detection/open_drone_id_parser.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/wifi_beacon_rid_parser.c` | `a8805fa5de79262f6cc09bfd6a9c80a4fd3639134cf8b9c3f69c470dd974cf7a` | `scanner/main/detection/wifi_beacon_rid_parser.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/wifi_beacon_rid_parser.h` | `c4a0d81777e1e206ef8581f11005cf9b24de053b62dc9abb72920fd3c2f91f18` | `scanner/main/detection/wifi_beacon_rid_parser.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/wifi_oui_database.c` | `31215593b48216f73557fff151b3336ec9fd6c340952e429e98ddfc18590b1e1` | `scanner/main/detection/wifi_oui_database.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/wifi_oui_database.h` | `8707450d752aa1c15eac5eff2ff28594d204f37b08798f8021508fe2e37a1c87` | `scanner/main/detection/wifi_oui_database.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/wifi_scanner.c` | `464b907860bded184f1f03c7d73431e097bf01647eb6d278d6a41c2d671dba1e` | `scanner/main/detection/wifi_scanner.c` | Device-only: backend priorities and detection sink; calibration/Easter/direct UART removed; AP/probe/association/anomaly/lock-on retained. |
| `esp32/scanner/main/detection/wifi_scanner.h` | `0602f47c3a9937bab566f0a96b4e69cfa0a37c45a172ba7855243680c224ff8e` | `scanner/main/detection/wifi_scanner.h` | Device-only API no longer accepts/retains caller queues. |
| `esp32/scanner/main/detection/wifi_ssid_patterns.c` | `37c69f69ec5ccfee59350da29c8a42caac34671857968cc39350a330d1360517` | `scanner/main/detection/wifi_ssid_patterns.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/detection/wifi_ssid_patterns.h` | `dd5a5886c5cbd0287c037a3a54891d38614f4f089a02518c84e595cb862d842e` | `scanner/main/detection/wifi_ssid_patterns.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/scanner/main/comms/uart_ota.c` | `c9335da1ef5f6f1dafb67b8c66df057dc64633598744d38bb53af7c9c37da8f6` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/comms/uart_ota.h` | `7e78ab2c0882339f5767d781a7c44999d698678bc2e4e5b342b80599a1325db0` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/comms/uart_tx.c` | `3ed7f5e6880c4a21acac4899c11a1e87cc8748746f13f22f01cbdccf413b9852` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/comms/uart_tx.h` | `a6eeb921ac35cca3e256970f30afdac7c05415e81459b80e16fcd0cae4001140` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/core/calibration_mode.c` | `ea36676900e648f3ca3e8042829292c90b8505974a27939fd319570eaa282f84` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/core/calibration_mode.h` | `7d194d36d92a258b2c54852c44b40cd540820e5929e589962ede1a287372ad34` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/core/scanner_rollback.c` | `3707d12b6cff4f05a8700292a09de564cdb65b3a9a8da4842bfeaf7bcb09f2ed` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/core/scanner_rollback.h` | `c3e19f9ee3c51557931138d9917903f107f07f4075e977752a24e1f76c73d2ea` | evidence only | Evidence only; no local compile path. |
| `esp32/scanner/main/core/task_priorities.h` | `0ba7ae6ae8b9e25cebc4511f77ed9cefdf540d28233e001e7afe09bed7879527` | `scanner/main/core/backend_task_priorities.h` | Reduced backend scanner priority/stack/core contract. |
| `esp32/shared/badge_ble_rssi_policy.h` | `9aa34e8e911aac2d953baeeb5c9410d552a6ddadcbcae1eb67de77394da962ec` | `shared/backend_ble_rssi_policy.h` | Renamed backend RSSI policy; removed badge naming. |
| `esp32/shared/ble_investigation_protocol.c` | `0fa47b46915ac97d4f7334b6d38ae7a1d9097d50c5a885d677d56f1cd14498a5` | `shared/ble_investigation_protocol.c` | Uses `backend_uart_protocol.h`; not native-linked. |
| `esp32/shared/ble_investigation_protocol.h` | `9fc6523b7f4c9aa05f1a60d0625c7f5865044bfc452b7d9eddfab8063f6385d0` | `shared/ble_investigation_protocol.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/ble_investigation_types.h` | `3b2376d927c706e3632992fa4e17526638d407ff9d4f60e975b074dbe03bba92` | `shared/ble_investigation_types.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/constants.h` | `bac1573cb618028103892592fee7bdbbc02d65c514d25e21a5b10164f1e4934a` | `shared/constants.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/detection_policy.c` | `36a260c92af08898bc980f1f4c98d05f9223d7e48f5930f1f9f3c31819a137f6` | `shared/detection_policy.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/detection_policy.h` | `093fc455612da792b4b29668d5c4968f307bb8dafa44de3502f557cca9b7774c` | `shared/detection_policy.h` | Fixed backend topology macro replaces `FOF_BADGE_VARIANT`. |
| `esp32/shared/detection_types.h` | `e3ffb0c98f3616e7c8bae0c30fade33fc26264f75799a8a7bfd10021b8ef0566` | `shared/detection_types.h` | Backend skimmer gate emits both threat kinds under `FOF_BACKEND_FIRMWARE`. |
| `esp32/shared/firmware_image_contract.c` | `2989ea7d74c375ac7aa9ce41e58efd3a8c19e4ec429baebcf0cdbfd8b71a7eb3` | `shared/firmware_image_contract.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/firmware_image_contract.h` | `133fd480d232ff721aee8dd7b2e92215de381454b2f0e799137b245de0fc524d` | `shared/firmware_image_contract.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/firmware_json_schema.c` | `0c2540df27ffe9fd05d8b85e229376ab721cf878967a4438902d623eff7b8ff8` | `shared/firmware_json_schema.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/firmware_json_schema.h` | `08c2cced1e67858c4c242e82f45b5381413a1d06f30b0886859d95e71c35414c` | `shared/firmware_json_schema.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/firmware_operation_token.c` | `7a810ba291a6b2812ed353a72a0dd19f804c7099d0b9d90a53c0816dcdd56e12` | `shared/firmware_operation_token.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/firmware_operation_token.h` | `c37c6638dc9efe409a09983293349120ce0c622aa222f726dc3b537bd437906d` | `shared/firmware_operation_token.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/firmware_version_order.c` | `715a8c855b2ac57cead91f9f934fa059ed17834d8a19ce8ff5241888e000b9f4` | `shared/firmware_version_order.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/firmware_version_order.h` | `77e34533fd26d93535582fb92f1428692cbba939f208059344462ed80ff3f54a` | `shared/firmware_version_order.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/privacy_rf_signatures.c` | `73b35973be25fcdf9a4c2dcfb14c5622e6334b2f74e9f5ea81958615387eb24e` | `shared/privacy_rf_signatures.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/privacy_rf_signatures.h` | `919c4d9f83dc5be5cfb517eaabc660800a354a343f0fc38cdc27c5ce97b917af` | `shared/privacy_rf_signatures.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/psram_alloc.c` | `25ade6de28b8a817c468b25fba0a5b35b311b44b4d95bdb913556882cbc4a591` | `shared/psram_alloc.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/psram_alloc.h` | `b398feb9e37c7be37c0aee84894e6afe7264ec1611a039e2c2e9dbbc67968602` | `shared/psram_alloc.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/rssi_distance.h` | `03688bc7da731b18ceefa2afd4ed3e34628d80f102ea9fdafc4b71e53c537cc5` | `shared/rssi_distance.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/scanner_uart_line_framer.c` | `e9825173da30d0d500e81561dae33519dec755a2773068549514b2ed3cb23b85` | `shared/scanner_uart_line_framer.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/scanner_uart_line_framer.h` | `11429ba5ee4aefb2bd22d04e8f4b6fdb00b071552b1845a45ee13ca4f16a3c75` | `shared/scanner_uart_line_framer.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/time_sync_policy.c` | `d97d5dc866feff7f58869fc78a1b5ee52bfc7ab3d1c82cd32eed1075b603df00` | `shared/time_sync_policy.c` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/time_sync_policy.h` | `04a7daeef922e8b56cef2e5a760dff7802a7bda80289fc3f9b644b29052ba1f8` | `shared/time_sync_policy.h` | Verbatim backend-owned build copy; immutable donor remains uncompiled. |
| `esp32/shared/badge_threat_policy.c` | `085ef1cd85400d52cd8d917f70ab312ecec1d78ad5099628f2e5b23e246db9df` | evidence only | Evidence only; no local compile path. |
| `esp32/shared/badge_threat_policy.h` | `ce6e8cf306ff936011be728664af2d5c3cb793979018b52c48e962c6eddcdbe9` | evidence only | Evidence only; no local compile path. |
| `esp32/shared/uart_protocol.h` | `d7b62f273f12e6a8454a7ed31d678ac21a51894833e81d78b1625548c9573029` | `shared/backend_uart_protocol.h` | Evidence-only badge/pin/variant donor; backend header copies only consumed commands, keys, properties, and OTA framing. |
