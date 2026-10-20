"""PSA-ramp property tests.

These check structural properties (linearity, plateau, seasoning offset,
speed scaling) rather than single golden values, so they stay meaningful
as the model gains behavioral components.
"""

import numpy as np
import pytest

import prepay


def _smm_row(model, age, horizon):
    """SMM path for a single pool of the given age."""
    pools = {"age": np.array([age], dtype=np.uint32)}
    return prepay.project(model, pools, horizon)[0]


def _cpr_row(model, age, horizon):
    """Annualized CPR path for a single pool of the given age."""
    return prepay.smm_to_cpr(_smm_row(model, age, horizon))


def test_ramp_is_linear_from_new(psa100):
    # 100 PSA: CPR = 0.2% * month, months 1..30.
    cpr = _cpr_row(psa100, age=0, horizon=30)
    np.testing.assert_allclose(cpr, 0.002 * np.arange(1, 31), atol=1e-12)


def test_plateau_at_six_percent(psa100):
    # Month 30 onward is flat 6% CPR.
    cpr = _cpr_row(psa100, age=0, horizon=40)
    np.testing.assert_allclose(cpr[29:], 0.06, atol=1e-12)


def test_seasoning_offset_shifts_the_curve(psa100):
    # A pool aged 12 months starts where a new pool's month 13 would be.
    new = _cpr_row(psa100, age=0, horizon=24)
    seasoned = _cpr_row(psa100, age=12, horizon=12)
    np.testing.assert_allclose(seasoned, new[12:24], atol=1e-12)


@pytest.mark.parametrize("speed", [0.5, 1.0, 2.0, 4.0])
def test_cpr_scales_linearly_with_speed(speed):
    # CPR (not SMM) is what scales with the PSA multiple.
    base = _cpr_row(prepay.psa(1.0), age=0, horizon=20)
    scaled = _cpr_row(prepay.psa(speed), age=0, horizon=20)
    np.testing.assert_allclose(scaled, speed * base, atol=1e-12)


def test_zero_speed_gives_no_prepayment():
    np.testing.assert_array_equal(_smm_row(prepay.psa(0.0), age=0, horizon=12), 0.0)


def test_smm_is_monotonic_through_the_ramp(psa100):
    smm = _smm_row(psa100, age=0, horizon=30)
    assert np.all(np.diff(smm) > 0)


def test_smm_stays_in_unit_interval(mixed_pools):
    smm = prepay.project(prepay.psa(10.0), mixed_pools, horizon=36)
    assert np.all(smm >= 0.0) and np.all(smm <= 1.0)


def test_batch_matches_individual(psa100, mixed_pools):
    # Pools are independent: a batch row equals that pool projected alone.
    batch = prepay.project(psa100, mixed_pools, horizon=24)
    for i, age in enumerate(mixed_pools["age"]):
        np.testing.assert_allclose(batch[i], _smm_row(psa100, age, 24), atol=1e-15)


def test_roundtrip_smm_cpr(psa100, mixed_pools):
    smm = prepay.project(psa100, mixed_pools, horizon=12)
    np.testing.assert_allclose(prepay.cpr_to_smm(prepay.smm_to_cpr(smm)), smm, atol=1e-15)


def test_negative_speed_rejected():
    with pytest.raises(ValueError):
        prepay.psa(-1.0)
