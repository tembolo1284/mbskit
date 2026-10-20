"""Shared pytest fixtures.

If the native extension has not been built yet, skip collection of this
directory quietly rather than failing every test with an ImportError.
"""

import numpy as np
import pytest

try:
    import prepay
except ImportError:  # pragma: no cover
    prepay = None
    # Skip the Python tests; the C smoke test is unaffected.
    collect_ignore_glob = ["test_*.py"]


@pytest.fixture
def psa100():
    """100 PSA model."""
    return prepay.psa(1.0)


@pytest.fixture
def new_pool():
    """Single brand-new pool (age 0)."""
    return {"age": np.array([0], dtype=np.uint32)}


@pytest.fixture
def mixed_pools():
    """Three pools spanning the ramp: new, mid-ramp, past the plateau."""
    return {
        "age": np.array([0, 15, 60], dtype=np.uint32),
        "original_term": np.array([360, 360, 180], dtype=np.uint32),
        "balance": np.array([250_000.0, 500_000.0, 125_000.0]),
        "wac": np.array([0.065, 0.055, 0.070]),
        "note_rate": np.array([0.060, 0.050, 0.065]),
    }
