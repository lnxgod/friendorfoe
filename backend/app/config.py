"""Application configuration via environment variables."""

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    """App settings loaded from environment variables."""

    # App
    app_name: str = "Friend or Foe Backend"
    debug: bool = False

    # OpenSky Network API
    opensky_base_url: str = "https://opensky-network.org/api"
    opensky_username: str | None = None
    opensky_password: str | None = None

    # Redis
    redis_url: str = "redis://localhost:6379/0"
    cache_ttl_seconds: int = 10

    # Database (SQLite default, override with DATABASE_URL env var for PostgreSQL)
    database_url: str = "sqlite+aiosqlite:///friendorfoe.db"

    # ADS-B data sources (fallback chain)
    adsbfi_base_url: str = "https://opendata.adsb.fi/api"
    airplanes_live_base_url: str = "https://api.airplanes.live"
    adsb_one_base_url: str = "https://api.adsb.one"
    adsb_lol_base_url: str = "https://api.adsb.lol"

    # Aircraft enrichment
    planespotters_base_url: str = "https://api.planespotters.net/pub/photos/hex"
    hexdb_base_url: str = "https://hexdb.io"

    # Defaults
    default_radius_nm: float = 50.0
    max_radius_nm: float = 250.0

    model_config = {"env_file": ".env", "env_file_encoding": "utf-8"}


settings = Settings()
