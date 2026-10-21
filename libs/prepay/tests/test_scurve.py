"""Behavioral properties of the refi S-curve model.

These are property tests, not golden values. The point is to pin the *shape*
the model promises — monotone in incentive, bracketed by floor and ceiling,
damped by seasoning — so a future refactor (burnout, a different functional
form) has to break a stated property rather than a magic number.
"""

import numpy as np
import pytest

import prepay


HORIZON = 24
FLOOR = 0.05
CEILING = 0.45
MIDPOINT = 0.0075   # 75bp
STEEPNESS = 400.0


def flat_scenario(rate, months=HORIZON, scenario_id=0):
    return prepay.scenario(np.full(months, rate), scenario_id=scenario_id)


def one_pool(wac, age=0):
    return {
        "age": np.array([age], dtype=np.uint32),
        "wac": np.array([wac], dtype=np.float64),
        "note_rate": np.array([wac], dtype=np.float64),
        "balance": np.array([1_000_000.0], dtype=np.float64),
    }


def scurve(seasoning_months=0.0, **kw):
    params = dict(
        floor=FLOOR,
        ceiling=CEILING,
        midpoint=MIDPOINT,
        steepness=STEEPNESS,
        seasoning_months=seasoning_months,
    )
    params.update(kw)
    return prepay.refi_scurve(**params)


def cpr_at(incentive, *, mortgage_rate=0.05, seasoning_months=0.0, age=0):
    """Annual CPR in month 0 for a pool with the given refi incentive."""
    model = scurve(seasoning_months=seasoning_months)
    with flat_scenario(mortgage_rate) as scen:
        smm = prepay.project(
            model, one_pool(mortgage_rate + incentive, age=age),
            horizon=HORIZON, scenario=scen,
        )
    return prepay.smm_to_cpr(smm)[0, 0]


# ---- bracketing ---------------------------------------------------------

def test_cpr_stays_between_floor_and_ceiling():
    # Sweep from deeply out of the money to deeply in; the logistic is bounded,
    # so every point must land inside [floor, ceiling] regardless of input.
    for incentive in np.linspace(-0.05, 0.05, 41):
        cpr = cpr_at(incentive)
        assert FLOOR <= cpr <= CEILING, f"incentive={incentive}: cpr={cpr}"


def test_deep_out_of_the_money_approaches_floor():
    # 500bp out of the money with steepness 400 puts the logistic exponent
    # near -2000; CPR should be indistinguishable from the turnover floor.
    assert cpr_at(-0.05) == pytest.approx(FLOOR, abs=1e-9)


def test_deep_in_the_money_approaches_ceiling():
    assert cpr_at(0.05) == pytest.approx(CEILING, abs=1e-9)


def test_midpoint_sits_at_half_height():
    # By construction, incentive == midpoint puts the logistic at exactly 0.5.
    assert cpr_at(MIDPOINT) == pytest.approx((FLOOR + CEILING) / 2.0, rel=1e-12)


# ---- monotonicity -------------------------------------------------------

def test_cpr_is_monotone_increasing_in_incentive():
    incentives = np.linspace(-0.03, 0.03, 61)
    cprs = np.array([cpr_at(x) for x in incentives])
    assert np.all(np.diff(cprs) >= 0.0), "CPR must never fall as incentive rises"
    # And it must actually move — a constant curve would pass the check above.
    assert cprs[-1] - cprs[0] > 0.3


def test_lower_rates_prepay_faster():
    # Same pool, two rate environments. This is the economically meaningful
    # direction: a rate rally must raise prepayments, never lower them.
    model = scurve()
    pools = one_pool(0.065)
    speeds = []
    for rate in (0.07, 0.065, 0.06, 0.05, 0.04):
        with flat_scenario(rate) as scen:
            smm = prepay.project(model, pools, horizon=HORIZON, scenario=scen)
        speeds.append(prepay.smm_to_cpr(smm)[0, 0])
    assert np.all(np.diff(speeds) > 0.0)


# ---- seasoning ----------------------------------------------------------

def test_seasoning_damps_a_new_pool():
    seasoned = cpr_at(0.02, seasoning_months=0.0)
    new = cpr_at(0.02, seasoning_months=30.0)
    # Month 0 of a brand-new pool gets 1/30 of the ramp.
    assert new < seasoned
    assert new == pytest.approx(seasoned / 30.0, rel=1e-12)


def test_seasoning_ramp_is_linear_then_flat():
    model = scurve(seasoning_months=30.0)
    with flat_scenario(0.05, months=40) as scen:
        smm = prepay.project(model, one_pool(0.07), horizon=40, scenario=scen)
    cpr = prepay.smm_to_cpr(smm)[0]

    # Months 0..29 ramp linearly in (t+1)/30; month 29 onward is fully seasoned.
    ratios = cpr[:30] / cpr[29]
    expected = (np.arange(30) + 1.0) / 30.0
    np.testing.assert_allclose(ratios, expected, rtol=1e-12)
    np.testing.assert_allclose(cpr[30:], cpr[29], rtol=1e-12)


def test_an_already_seasoned_pool_is_not_damped():
    # A 60-month-old pool is past a 30-month ramp from month 0.
    assert cpr_at(0.02, seasoning_months=30.0, age=60) == pytest.approx(
        cpr_at(0.02, seasoning_months=0.0), rel=1e-12
    )


def test_seasoning_at_or_below_one_month_is_disabled():
    for s in (0.0, 1.0):
        assert cpr_at(0.02, seasoning_months=s) == pytest.approx(
            cpr_at(0.02, seasoning_months=0.0), rel=1e-12
        )


# ---- path dependence (or rather, its absence) ---------------------------

def test_model_is_currently_path_independent():
    # Two pools, identical except for the rate path that came BEFORE the
    # projection — which the model cannot see. A pool that has already lived
    # through a refi wave should burn out and prepay slower, but today it
    # doesn't. This test documents that gap; burnout will invert it.
    model = scurve()
    pools = one_pool(0.065)

    with flat_scenario(0.04) as rally:
        fast = prepay.project(model, pools, horizon=HORIZON, scenario=rally)

    # Same terminal environment, no memory of how we got here.
    path = np.concatenate([np.full(12, 0.04), np.full(HORIZON, 0.04)])
    with prepay.scenario(path) as after_rally:
        again = prepay.project(model, pools, horizon=HORIZON, scenario=after_rally)

    np.testing.assert_allclose(fast, again, rtol=0, atol=0)


# ---- flat-scenario equivalence ------------------------------------------

def test_zero_range_scurve_matches_const_cpr():
    # floor == ceiling collapses the logistic entirely, so the S-curve must
    # reduce to a flat CPR model. A useful check that the plumbing around the
    # logistic isn't contributing anything of its own.
    model = scurve(floor=0.12, ceiling=0.12)
    with flat_scenario(0.05) as scen:
        got = prepay.project(model, one_pool(0.065), horizon=HORIZON, scenario=scen)
    want = prepay.project(prepay.const_cpr(0.12), one_pool(0.065), horizon=HORIZON)
    np.testing.assert_allclose(got, want, rtol=1e-15)


def test_constant_rate_path_gives_constant_smm():
    model = scurve()
    with flat_scenario(0.05) as scen:
        smm = prepay.project(model, one_pool(0.065), horizon=HORIZON, scenario=scen)
    np.testing.assert_allclose(smm[0], smm[0, 0], rtol=1e-15)


def test_rate_path_is_followed_month_by_month():
    # A step down in rates mid-path must show up as a step up in SMM at
    # exactly that month — this is what proves the model indexes the path by t.
    path = np.concatenate([np.full(6, 0.07), np.full(6, 0.04)])
    model = scurve()
    with prepay.scenario(path) as scen:
        smm = prepay.project(model, one_pool(0.065), horizon=12, scenario=scen)[0]
    np.testing.assert_allclose(smm[:6], smm[0], rtol=1e-15)
    np.testing.assert_allclose(smm[6:], smm[6], rtol=1e-15)
    assert smm[6] > smm[5] * 2.0
