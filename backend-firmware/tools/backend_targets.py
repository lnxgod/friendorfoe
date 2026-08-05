"""Immutable management identities for every supported backend firmware image."""

BACKEND_TARGETS = {
    "uplink-s3-backend": {
        "family": "badge_lite",
        "component": "uplink",
        "project": "fof_backend_uplink",
        "hardware": "seeed_xiao_esp32s3",
        "flash_bytes": 8 * 1024 * 1024,
        "app_capacity": 2 * 1024 * 1024,
        "cache_capacity": 2 * 1024 * 1024,
        "default": True,
    },
    "scanner-s3-combo-backend": {
        "family": "badge_lite",
        "component": "scanner",
        "project": "fof_backend_scanner",
        "hardware": "seeed_xiao_esp32s3",
        "flash_bytes": 8 * 1024 * 1024,
        "app_capacity": 2 * 1024 * 1024,
        "cache_capacity": None,
        "default": True,
    },
    "uplink-s3-fullsize-backend": {
        "family": "s3_fullsize",
        "component": "uplink",
        "project": "fof_backend_uplink_fullsize",
        "hardware": "esp32s3_n16r8_fullsize",
        "flash_bytes": 16 * 1024 * 1024,
        "app_capacity": 2 * 1024 * 1024,
        "cache_capacity": 3 * 1024 * 1024,
        "default": False,
    },
    "scanner-s3-combo-fullsize-backend": {
        "family": "s3_fullsize",
        "component": "scanner",
        "project": "fof_backend_scanner_fullsize",
        "hardware": "esp32s3_n16r8_fullsize",
        "flash_bytes": 16 * 1024 * 1024,
        "app_capacity": 3 * 1024 * 1024,
        "cache_capacity": None,
        "default": False,
    },
}
