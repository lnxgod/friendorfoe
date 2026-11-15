"""Shared pytest configuration and fixtures for backend tests."""

import pytest


@pytest.fixture
def configured_test_network(monkeypatch):
    """An operator may explicitly label their own network. Nothing ships pre-owned."""
    from app.services import rf_reference

    rf_reference._load()
    monkeypatch.setattr(rf_reference, "_ssid_patterns", [*rf_reference._ssid_patterns, {
        "pattern": "example-fixture-ssid*", "label": "Example test network",
        "device_class": "known_owned", "device_family": "operator_known_lab_device", "confidence": 0.75,
    }])
