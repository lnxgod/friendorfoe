"""Integration tests for the Friend or Foe backend API.

Tests cover:
- Health endpoint
- Nearby aircraft endpoint (response structure and validation)
- Bounding box calculation from lat/lon/radius
- Haversine distance calculation
- Unit conversion helpers

Run with:  pytest tests/test_api.py -v
"""

import math

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from app.main import app
from app.services.adsb import bbox_from_point, haversine_nm, meters_to_feet, ms_to_knots, ms_to_fpm


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest_asyncio.fixture
async def client():
    """Create an async test client for the FastAPI app."""
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac


# ---------------------------------------------------------------------------
# Health endpoint
# ---------------------------------------------------------------------------

class TestHealth:
    """Tests for GET /health."""

    @pytest.mark.asyncio
    async def test_health_returns_200(self, client: AsyncClient):
        resp = await client.get("/health")
        assert resp.status_code == 200

    @pytest.mark.asyncio
    async def test_health_response_has_required_fields(self, client: AsyncClient):
        resp = await client.get("/health")
        data = resp.json()
        assert "status" in data
        assert "version" in data
        assert "redis" in data
        assert data["status"] == "ok"
        assert data["version"] == "0.32.0"

    @pytest.mark.asyncio
    async def test_root_returns_200(self, client: AsyncClient):
        resp = await client.get("/")
        assert resp.status_code == 200
        data = resp.json()
        assert "app" in data
        assert "docs" in data


# ---------------------------------------------------------------------------
# Nearby aircraft endpoint
# ---------------------------------------------------------------------------

class TestNearbyAircraft:
    """Tests for GET /aircraft/nearby.

    These tests validate the API contract (request validation, response structure).
    They may hit the real OpenSky API or fail gracefully depending on network
    availability and Redis state -- that is expected for integration tests.
    """

    @pytest.mark.asyncio
    async def test_nearby_requires_lat_lon(self, client: AsyncClient):
        """Missing required lat/lon should return 422."""
        resp = await client.get("/aircraft/nearby")
        assert resp.status_code == 422

    @pytest.mark.asyncio
    async def test_nearby_rejects_invalid_lat(self, client: AsyncClient):
        """Latitude out of range should return 422."""
        resp = await client.get("/aircraft/nearby", params={"lat": 91.0, "lon": 0.0})
        assert resp.status_code == 422

    @pytest.mark.asyncio
    async def test_nearby_rejects_invalid_lon(self, client: AsyncClient):
        """Longitude out of range should return 422."""
        resp = await client.get("/aircraft/nearby", params={"lat": 0.0, "lon": 181.0})
        assert resp.status_code == 422

    @pytest.mark.asyncio
    async def test_nearby_rejects_excessive_radius(self, client: AsyncClient):
        """Radius beyond max_radius_nm should return 422."""
        resp = await client.get(
            "/aircraft/nearby",
            params={"lat": 40.0, "lon": -74.0, "radius_nm": 999.0},
        )
        assert resp.status_code == 422

    @pytest.mark.asyncio
    async def test_nearby_valid_request_returns_valid_structure(self, client: AsyncClient):
        """Valid request should return 200 with correct JSON structure,
        or 502 if the upstream ADS-B source is unreachable."""
        resp = await client.get(
            "/aircraft/nearby",
            params={"lat": 40.7128, "lon": -74.0060, "radius_nm": 10},
        )
        # Accept 200 (success) or 502 (upstream unavailable in test env)
        assert resp.status_code in (200, 502)

        if resp.status_code == 200:
            data = resp.json()
            assert "count" in data
            assert "lat" in data
            assert "lon" in data
            assert "radius_nm" in data
            assert "aircraft" in data
            assert isinstance(data["aircraft"], list)
            assert data["count"] == len(data["aircraft"])
            assert data["lat"] == pytest.approx(40.7128, abs=0.001)
            assert data["lon"] == pytest.approx(-74.0060, abs=0.001)


# ---------------------------------------------------------------------------
# Aircraft detail endpoint
# ---------------------------------------------------------------------------

class TestAircraftDetail:
    """Tests for GET /aircraft/{icao_hex}/detail."""

    @pytest.mark.asyncio
    async def test_detail_rejects_invalid_hex_length(self, client: AsyncClient):
        resp = await client.get("/aircraft/ABC/detail")
        assert resp.status_code == 400

    @pytest.mark.asyncio
    async def test_detail_rejects_non_hex(self, client: AsyncClient):
        resp = await client.get("/aircraft/ZZZZZZ/detail")
        assert resp.status_code == 400


# ---------------------------------------------------------------------------
# Bounding box calculation (unit-level, no HTTP)
# ---------------------------------------------------------------------------

class TestBboxFromPoint:
    """Tests for the bbox_from_point helper function."""

    def test_bbox_symmetric_around_center(self):
        lat, lon, radius = 40.0, -74.0, 50.0
        lamin, lamax, lomin, lomax = bbox_from_point(lat, lon, radius)

        # Should be symmetric around the center latitude
        assert pytest.approx(lat - lamin, abs=0.001) == pytest.approx(lamax - lat, abs=0.001)

        # All bounds should be finite
        assert math.isfinite(lamin)
        assert math.isfinite(lamax)
        assert math.isfinite(lomin)
        assert math.isfinite(lomax)

    def test_bbox_grows_with_radius(self):
        small = bbox_from_point(40.0, -74.0, 10.0)
        large = bbox_from_point(40.0, -74.0, 100.0)

        small_lat_span = small[1] - small[0]
        large_lat_span = large[1] - large[0]

        assert large_lat_span > small_lat_span

    def test_bbox_lat_delta_is_correct(self):
        """1 NM = 1/60 degree latitude. 60 NM radius should be 1 degree delta."""
        lat, lon = 45.0, 0.0
        lamin, lamax, lomin, lomax = bbox_from_point(lat, lon, 60.0)

        lat_delta = lamax - lat
        assert pytest.approx(lat_delta, abs=0.01) == 1.0

    def test_bbox_lon_wider_at_high_latitude(self):
        """Longitude degrees per NM grows at higher latitudes (more degrees needed)."""
        equator_bbox = bbox_from_point(0.0, 0.0, 50.0)
        high_lat_bbox = bbox_from_point(60.0, 0.0, 50.0)

        equator_lon_span = equator_bbox[3] - equator_bbox[2]
        high_lat_lon_span = high_lat_bbox[3] - high_lat_bbox[2]

        assert high_lat_lon_span > equator_lon_span


# ---------------------------------------------------------------------------
# Haversine distance (unit-level, no HTTP)
# ---------------------------------------------------------------------------

class TestHaversine:
    """Tests for the haversine_nm distance function."""

    def test_same_point_is_zero(self):
        dist = haversine_nm(40.0, -74.0, 40.0, -74.0)
        assert dist == pytest.approx(0.0, abs=0.001)

    def test_one_degree_lat_is_60_nm(self):
        """1 degree of latitude = 60 NM."""
        dist = haversine_nm(0.0, 0.0, 1.0, 0.0)
        assert dist == pytest.approx(60.0, abs=0.5)

    def test_known_distance_jfk_to_lax(self):
        """JFK (40.6413, -73.7781) to LAX (33.9425, -118.4081) ~ 2,145 NM."""
        dist = haversine_nm(40.6413, -73.7781, 33.9425, -118.4081)
        assert 2100 < dist < 2200

    def test_symmetry(self):
        """haversine(A, B) == haversine(B, A)."""
        d1 = haversine_nm(40.0, -74.0, 34.0, -118.0)
        d2 = haversine_nm(34.0, -118.0, 40.0, -74.0)
        assert d1 == pytest.approx(d2, abs=0.001)

    def test_antipodal_points(self):
        """Distance between antipodal points should be ~10,800 NM (half circumference)."""
        dist = haversine_nm(0.0, 0.0, 0.0, 180.0)
        assert 10700 < dist < 10900


# ---------------------------------------------------------------------------
# Unit conversion helpers (unit-level, no HTTP)
# ---------------------------------------------------------------------------

class TestUnitConversions:
    """Tests for meters_to_feet, ms_to_knots, ms_to_fpm."""

    def test_meters_to_feet_known_value(self):
        assert meters_to_feet(1000.0) == pytest.approx(3281.0, abs=1.0)

    def test_meters_to_feet_none(self):
        assert meters_to_feet(None) is None

    def test_ms_to_knots_known_value(self):
        # 1 m/s ~ 1.94 kts
        assert ms_to_knots(100.0) == pytest.approx(194.4, abs=0.5)

    def test_ms_to_knots_none(self):
        assert ms_to_knots(None) is None

    def test_ms_to_fpm_known_value(self):
        # 1 m/s ~ 196.85 fpm
        assert ms_to_fpm(10.0) == pytest.approx(1969.0, abs=1.0)

    def test_ms_to_fpm_none(self):
        assert ms_to_fpm(None) is None
