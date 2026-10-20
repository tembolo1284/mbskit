"""Mortgage prepayment model library.

Thin, numpy-native Python API over the native `_prepay` extension.

Example
-------
    >>> import numpy as np, prepay
    >>> m = prepay.psa(1.0)                      # 100 PSA
    >>> pools = {"age": np.array([0, 12], dtype=np.uint32)}
    >>> smm = prepay.project(m, pools, horizon=6)   # -> (2, 6) float64
    >>> cpr = prepay.smm_to_cpr(smm)
"""

from __future__ import annotations

import numpy as np

from . import _prepay
from ._prepay import ModelType, Model

__all__ = [
    "ModelType",
    "Model",
    "psa",
    "const_cpr",
    "project",
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


def project(model: Model, pools, horizon: int) -> np.ndarray:
    """Project SMM for each pool over `horizon` months.

    `pools` is a mapping or DataFrame with at least an ``age`` column;
    ``original_term`` defaults to 360 and ``balance``/``wac``/``note_rate``
    default to 0. Returns an ``(n_pools, horizon)`` float64 array of SMM.
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
        cols["original_term"],
        cols["age"],
        cols["balance"],
        cols["wac"],
        cols["note_rate"],
        int(horizon),
    )


def smm_to_cpr(smm):
    """Convert single monthly mortality to annualized CPR."""
    return 1.0 - (1.0 - np.asarray(smm, dtype=np.float64)) ** 12


def cpr_to_smm(cpr):
    """Convert annualized CPR to single monthly mortality."""
    return 1.0 - (1.0 - np.asarray(cpr, dtype=np.float64)) ** (1.0 / 12.0)
