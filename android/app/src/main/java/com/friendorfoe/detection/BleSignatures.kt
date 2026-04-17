package com.friendorfoe.detection

/**
 * Central BLE signature registry.
 *
 * Mirrors `esp32/scanner/main/detection/ble_fingerprint.c` company-ID and
 * service-UUID constants so Android + firmware classification stay in lock-step.
 * Also hosts the furiousMAC-derived Apple Continuity layout (sub-types,
 * data-flags bits, Nearby-Action sub-types) and Marauder-imported signatures.
 *
 * Do not duplicate these constants elsewhere in the module — reference them
 * from here. Adding a new manufacturer? Add the CID here first.
 */
object BleSignatures {

    // ── Core BLE Company IDs (BT SIG assigned numbers) ─────────────────────

    const val CID_APPLE             = 0x004C
    const val CID_MICROSOFT         = 0x0006
    const val CID_SAMSUNG           = 0x0075
    const val CID_GOOGLE            = 0x00E0
    const val CID_DJI               = 0x2CA5
    const val CID_TILE              = 0x0059
    const val CID_META              = 0x01AB
    const val CID_META_TECH         = 0x058E
    /** Luxottica — frame manufacturer for Ray-Ban Meta & Oakley Meta.
     *  Marauder matches on this; firmware v0.58 matches on this; Android did not until now. */
    const val CID_LUXOTTICA         = 0x0D53
    const val CID_FLIPPER           = 0x0E29
    const val CID_BOSE              = 0x009E
    const val CID_BOSE_ALT          = 0x009F
    const val CID_JBL               = 0x0057
    const val CID_SONY              = 0x012D
    const val CID_FITBIT            = 0x0108
    const val CID_GARMIN            = 0x0087
    const val CID_XIAOMI            = 0x038F
    const val CID_HUAWEI            = 0x027D
    const val CID_AMAZON            = 0x0171
    const val CID_SONOS             = 0x0236
    const val CID_IKEA              = 0x0311
    const val CID_TESLA             = 0x04F6
    const val CID_GOPRO             = 0x02DF
    const val CID_PARROT            = 0x0289
    const val CID_AUTEL             = 0x0986
    const val CID_NINTENDO          = 0x0578
    const val CID_AXON              = 0x04D8
    const val CID_AXON_ALT          = 0x034D
    const val CID_SEGWAY            = 0x06A1
    const val CID_DEXCOM            = 0x0267
    const val CID_SNAP              = 0x03C2
    const val CID_VUZIX             = 0x060C
    const val CID_BRILLIANT_LABS    = 0x09B1

    // ── 16-bit Service UUIDs ───────────────────────────────────────────────

    const val SVC_TILE_1            = 0xFEED
    const val SVC_TILE_2            = 0xFEEC
    const val SVC_GOOGLE_FASTPAIR   = 0xFE2C
    const val SVC_APPLE_FIND_MY     = 0xFD6F
    const val SVC_SAMSUNG_TAG_1    = 0xFD59
    const val SVC_SAMSUNG_TAG_2    = 0xFD5A
    const val SVC_SAMSUNG_TAG_LOST = 0xFD69
    const val SVC_META_RAYBAN_G2    = 0xFD5F
    const val SVC_META_1            = 0xFEB7
    const val SVC_META_2            = 0xFEB8
    const val SVC_EDDYSTONE         = 0xFEAA

    // ── Apple Continuity sub-types (mfr_data[2] after the 2-byte CID) ──────
    // Mirrors APPLE_TYPE_* in esp32/scanner/main/detection/ble_fingerprint.c

    const val APPLE_AIRDROP         = 0x05
    const val APPLE_HOMEKIT         = 0x06
    const val APPLE_AIRPODS         = 0x07
    const val APPLE_AIRPLAY         = 0x09
    const val APPLE_HANDOFF         = 0x0C
    const val APPLE_TETHER_SOURCE   = 0x0D  // iPhone broadcasting Personal Hotspot
    const val APPLE_TETHER_TARGET   = 0x0E  // Mac/iPad using someone else's hotspot
    const val APPLE_NEARBY_ACTION   = 0x0F
    const val APPLE_NEARBY_INFO     = 0x10
    const val APPLE_FINDMY          = 0x12

    /** AirTag discriminator — mfr_data[3] byte for FindMy (0x12) advertisements. */
    const val APPLE_AIRTAG_LEN_BYTE = 0x19

    // ── Apple Nearby Info / Nearby Action data-flags byte ──────────────────
    // Offset +7 of Apple mfr_data for types 0x0F / 0x10.
    // Bits mirror uart_protocol.h JSON_KEY_BLE_APPLE_FLAGS exactly.

    const val APPLE_FLAG_AIRPODS_IN   = 0x01
    const val APPLE_FLAG_WIFI_ON      = 0x02
    const val APPLE_FLAG_WATCH_PAIRED = 0x04
    const val APPLE_FLAG_ICLOUD       = 0x08
    const val APPLE_FLAG_AUTH_TAG     = 0x10
    const val APPLE_FLAG_SCREEN_ON    = 0x20

    // ── Apple Nearby Action sub-types (byte +3 of type 0x0F payload) ───────
    // Per furiousMAC reverse-engineering research.

    const val ACTION_APPLETV_SETUP   = 0x01
    const val ACTION_APPLETV_PAIR    = 0x04
    const val ACTION_INTERNET_RELAY  = 0x06
    const val ACTION_DEVELOPER_TOOLS = 0x09
    const val ACTION_WIFI_PASSWORD   = 0x0B
    const val ACTION_REPAIR          = 0x0E
    const val ACTION_SETUP_DEVICE    = 0x13
    const val ACTION_TRANSFER_NUMBER = 0x14
    const val ACTION_VISION_PRO      = 0x20

    fun nearbyActionName(sub: Int): String? = when (sub) {
        ACTION_APPLETV_SETUP   -> "Apple TV Setup"
        ACTION_APPLETV_PAIR    -> "Apple TV Pair"
        ACTION_INTERNET_RELAY  -> "Internet Relay"
        ACTION_DEVELOPER_TOOLS -> "Developer Tools"
        ACTION_WIFI_PASSWORD   -> "Wi-Fi Password Share"
        ACTION_REPAIR          -> "Repair"
        ACTION_SETUP_DEVICE    -> "Setup New Device"
        ACTION_TRANSFER_NUMBER -> "Transfer Number"
        ACTION_VISION_PRO      -> "Vision Pro Setup"
        else -> null
    }

    /** Decode the data-flags byte into human-readable bullet list (only set bits). */
    fun nearbyFlagLabels(flags: Int): List<String> = buildList {
        if (flags and APPLE_FLAG_AIRPODS_IN   != 0) add("AirPods in")
        if (flags and APPLE_FLAG_WIFI_ON      != 0) add("Wi-Fi on")
        if (flags and APPLE_FLAG_WATCH_PAIRED != 0) add("Watch paired")
        if (flags and APPLE_FLAG_ICLOUD       != 0) add("iCloud primary")
        if (flags and APPLE_FLAG_AUTH_TAG     != 0) add("Auth tag")
        if (flags and APPLE_FLAG_SCREEN_ON    != 0) add("Screen active")
    }

    // ── Advertising Flags (AD type 0x01) ───────────────────────────────────

    /** Simultaneous LE + BR/EDR Host. Set → dual-mode (phone/laptop/headset);
     *  clear → pure-BLE (AirTag, Tile, beacon). Dual-mode discriminator. */
    const val ADV_FLAG_SIMUL_LE_BR_EDR_HOST = 0x08

    // ── AD type IDs we care about ──────────────────────────────────────────

    const val AD_FLAGS                  = 0x01
    const val AD_INCOMPLETE_UUID16      = 0x02
    const val AD_COMPLETE_UUID16        = 0x03
    const val AD_SHORTENED_LOCAL_NAME   = 0x08
    const val AD_COMPLETE_LOCAL_NAME    = 0x09
    const val AD_TX_POWER               = 0x0A
    const val AD_SERVICE_DATA_UUID16    = 0x16
    const val AD_APPEARANCE             = 0x19
    const val AD_MANUFACTURER_SPECIFIC  = 0xFF

    // ── Marauder-imported signatures ───────────────────────────────────────

    /** Pwnagotchi default BSSID — deliberately attention-getting.
     *  Emits as ATTACK_TOOL threat in WifiAnomalyDetector. */
    const val PWNAGOTCHI_BSSID = "DE:AD:BE:EF:DE:AD"

    // ── Microsoft Swift Pair scenario byte table ───────────────────────────
    // Mirrors the firmware's v0.59 decoder.

    /** Map Swift Pair scenario byte (mfr_data[3] after Msft CID + 0x03 beacon type)
     *  to a human-readable device-type label. */
    fun microsoftSwiftPairLabel(scenario: Int): String? = when (scenario) {
        0x01 -> "Surface Pen"
        0x02 -> "Xbox Controller"
        0x03 -> "Precision Mouse"
        0x04 -> "Surface Headphones"
        0x05 -> "Surface Earbuds"
        0x06 -> "Surface Laptop"
        else -> null
    }

    // ── FNV-1a ─────────────────────────────────────────────────────────────

    const val FNV1A_OFFSET_32   = 0x811c9dc5.toInt()
    const val FNV1A_PRIME_32    = 0x01000193
}
