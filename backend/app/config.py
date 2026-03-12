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

    # PostgreSQL
    database_url: str = "postgresql+asyncpg://friendorfoe:friendorfoe@localhost:5432/friendorfoe"

    # Aircraft enrichment
    planespotters_base_url: str = "https://api.planespotters.net/pub/photos/hex"

    # Defaults
    default_radius_nm: float = 50.0
    max_radius_nm: float = 250.0

    model_config = {"env_file": ".env", "env_file_encoding": "utf-8"}


settings = Settings()
