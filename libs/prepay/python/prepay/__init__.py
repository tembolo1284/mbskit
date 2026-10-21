"""Mortgage prepayment model library.

Thin, numpy-native Python API over the native `_prepay` extension.

Example
-------
    >>> import numpy as np, prepay
    >>> m = prepay.psa(1.0)                      # 100 PSA
    >>> pools = {"age": np.array([0, 12], dtype=np.uint32)}
    >>> smm = prepay.project(m, pools, horizon=6)   # -> (2, 6) float64
    >>> cpr = prepay.smm_to_cpr(smm)

Rate-sensitive models need a scenario:

    >>> rates = np.full(360, 0.05)
    >>> with prepay.scenario(rates, scenario_id=1) as scen:
    ...     m = prepay.refi_scurve()
    ...     pools = {"age": np.zeros(1, np.uint32), "wac": np.array([0.065])}
    ...     smm = prepay.project(m, pools, horizon=120, scenario=scen)
"""

from __future__ import annotations

import numpy as np

from . import _prepay
from ._prepay import ModelType, Model, Scenario

__all__ = [
    "ModelType",
    "Model",
    "Scenario",
    "psa",
    "const_cpr",
    "refi_scurve",
    "scenario",
    "project",
    "provenance",
    "smm_to_cpr",
    "cpr_to_smm",
    "version",
    "__version__",
]

__version__ = _prepay.__version__


def version() -> int:
    """Runtime version of the linked libprepay (major<<16 | minor<<8 | patch)."""
    return _prepay.version()


def psa(speed: float = 1.0, model_version: int = 0) -> Model:
    """PSA-ramp model. `speed` is the multiple (1.0 == 100 PSA)."""
    return Model(ModelType.PSA, float(speed), int(model_version))


def const_cpr(annual_cpr: float, model_version: int = 0) -> Model:
    """Flat annual-CPR model. `annual_cpr` in [0, 1]."""
    return Model(ModelType.CONST_CPR, float(annual_cpr), int(model_version))


def refi_scurve(
    floor: float = 0.05,
    ceiling: float = 0.45,
    midpoint: float = 0.0075,
    steepness: float = 400.0,
    seasoning_months: float = 0.0,
    model_version: int = 0,
) -> Model:
    """Logistic refi S-curve model. Requires a scenario at projection time.

    Incentive is ``wac - mortgage_rate[t]`` in annual decimal, so `midpoint`
    of 0.0075 is 75bp and `steepness` lands near 400 rather than 4. The
    defaults are a plausible agency 30-year shape, not a calibration.
    Set `seasoning_months` above 1 to damp the first months of a new pool.
    """
    return Model(
        float(floor),
        float(ceiling),
        float(midpoint),
        float(steepness),
        float(seasoning_months),
        int(model_version),
    )


def scenario(mortgage_rate, scenario_id: int = 0) -> Scenario:
    """Build a rate scenario from a 1-D path of annual-decimal mortgage rates.

    The path is copied into the library, so `mortgage_rate` may be a temporary.
    Usable as a context manager for deterministic release; otherwise the handle
    is freed when the object is collected.
    """
    rates = np.ascontiguousarray(np.asarray(mortgage_rate), dtype=np.float64)
    if rates.ndim != 1:
        raise ValueError(f"mortgage_rate must be 1-D, got shape {rates.shape}")
    return Scenario(rates, int(scenario_id))


# Field -> (dtype, default). `age` has no default: it is required.
_FIELDS = {
    "original_term": (np.uint32, 360),
    "age": (np.uint32, None),
    "balance": (np.float64, 0.0),
    "wac": (np.float64, 0.0),
    "note_rate": (np.float64, 0.0),
}


def _column(pools, name, n, dtype, default):
    # `name in pools` works for both dict and pandas.DataFrame (column check).
    if name in pools:
        return np.ascontiguousarray(np.asarray(pools[name]), dtype=dtype)
    if default is None:
        raise KeyError(f"pools missing required field {name!r}")
    return np.full(n, default, dtype=dtype)


def project(model: Model, pools, horizon: int, scenario: Scenario | None = None):
    """Project SMM for each pool over `horizon` months.

    `pools` is a mapping or DataFrame with at least an ``age`` column;
    ``original_term`` defaults to 360 and ``balance``/``wac``/``note_rate``
    default to 0. `scenario` is required for rate-sensitive models and ignored
    by the rest. Returns an ``(n_pools, horizon)`` float64 array of SMM.
    """
    age = np.ascontiguousarray(np.asarray(pools["age"]), dtype=np.uint32)
    n = age.shape[0]

    cols = {
        name: (age if name == "age"
               else _column(pools, name, n, dtype, default))
        for name, (dtype, default) in _FIELDS.items()
    }

    return _prepay.project(
        model,
        scenario,
        cols["original_term"],
        cols["age"],
        cols["balance"],
        cols["wac"],
        cols["note_rate"],
        int(horizon),
    )


def provenance(model: Model, scenario: Scenario | None = None) -> dict:
    """Audit stamps for a projection: what produced a given result set.

    Returns the library version plus the caller-stamped `model_version` and
    `scenario_id`, so a saved SMM matrix can be traced back to the exact
    configuration that generated it.
    """
    v = version()
    out = {
        "library_version": f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}",
        "model_version": model.model_version,
        "scenario_id": None,
    }
    if scenario is not None:
        out["scenario_id"] = scenario.scenario_id
        out["n_months"] = scenario.n_months
    return out


def smm_to_cpr(smm):
    """Convert single monthly mortality to annualized CPR."""
    return 1.0 - (1.0 - np.asarray(smm, dtype=np.float64)) ** 12


def cpr_to_smm(cpr):
    """Convert annualized CPR to single monthly mortality."""
    return 1.0 - (1.0 - np.asarray(cpr, dtype=np.float64)) ** (1.0 / 12.0)
