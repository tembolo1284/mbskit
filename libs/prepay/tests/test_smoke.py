import numpy as np
import pytest

import prepay


def test_psa_ramp_age0():
    m = prepay.psa(1.0)  # 100 PSA
    smm = prepay.project(m, {"age": np.array([0], dtype=np.uint32)}, horizon=6)
    cpr = prepay.smm_to_cpr(smm[0])
    # 100 PSA ramps CPR by 0.2%/month.
    expected = np.array([0.002, 0.004, 0.006, 0.008, 0.010, 0.012])
    np.testing.assert_allclose(cpr, expected, atol=1e-12)


def test_psa_plateau_after_month_30():
    m = prepay.psa(1.0)
    smm = prepay.project(m, {"age": np.array([40], dtype=np.uint32)}, horizon=3)
    cpr = prepay.smm_to_cpr(smm[0])
    np.testing.assert_allclose(cpr, 0.06, atol=1e-12)  # flat 6%


def test_const_cpr_flat_and_shape():
    c = prepay.const_cpr(0.06)
    smm = prepay.project(c, {"age": np.array([0, 5], dtype=np.uint32)}, horizon=4)
    assert smm.shape == (2, 4)
    assert np.allclose(smm, smm[0, 0])


def test_bad_cpr_raises_valueerror():
    with pytest.raises(ValueError):
        prepay.const_cpr(1.5)


def test_zero_horizon_raises():
    m = prepay.psa(1.0)
    with pytest.raises(ValueError):
        prepay.project(m, {"age": np.array([0], dtype=np.uint32)}, horizon=0)
