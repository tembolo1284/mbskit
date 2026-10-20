# libprepay

Mortgage prepayment models in C++ behind a stable C ABI, with numpy-native Python bindings.

`prepay` projects prepayment speeds (SMM/CPR) for pools of mortgage loans. The
modeling core is modern C++; the public interface is a flat C ABI. Python is one
consumer of that ABI — an Excel add-in, a Rust service, or a CFFI script would be
equal citizens.

---

## Why the C ABI seam

The library is written as a real C++20 library internally, but exposes exactly one
public header (`include/prepay/prepay.h`) containing an `extern "C"` interface of
opaque handles and versioned POD structs.

This follows the approach in Armin Ronacher's
[Beautiful Native Libraries](https://lucumr.pocoo.org/2013/8/18/beautiful-native-libraries/),
with one deliberate deviation: rather than restricting the whole codebase to a
C-like subset of C++, the discipline applies *only at the boundary*. Internally we
use `std::span`, RAII, virtual dispatch, and exceptions freely. The single hard rule
is that **no exception ever unwinds across the `extern "C"` line** — every entry
point is a `try`/`catch` that converts to a status code.

What this buys:

- **One contract, many consumers.** Python, Excel, Rust, and C all bind the same
  seam. No language gets a privileged path.
- **ABI stability.** Structs are append-only (each leads with `struct_size`); new
  capability arrives as new functions, not changed signatures.
- **Clean symbol table.** Hidden visibility by default means only the six `prepay_*`
  entry points are exported — no `std::` or internal symbols leak.

---

## Quick start

```bash
git clone <your-remote> libprepay && cd libprepay
./build.sh                # venv -> native -> csmoke -> py -> pytest
```

Then:

```python
import numpy as np
import prepay

model = prepay.psa(1.0)                                  # 100 PSA
pools = {"age": np.array([0, 12, 60], dtype=np.uint32)}  # loan ages in months

smm = prepay.project(model, pools, horizon=360)          # (3, 360) float64
cpr = prepay.smm_to_cpr(smm)

print(cpr[0, :6])   # [0.002 0.004 0.006 0.008 0.010 0.012]
```

---

## Building

`build.sh` runs the pipeline in stages, individually or chained:

| Stage    | What it does                                                  |
|----------|---------------------------------------------------------------|
| `venv`   | create `./.venv`, install cmake/ninja/nanobind/scikit-build-core/numpy/pytest |
| `native` | configure + build `libprepay.so` (C ABI only) into `build-native/` |
| `csmoke` | compile and run `tests/smoke.c` against the shared library     |
| `py`     | build + editable-install the nanobind extension                |
| `pytest` | run the Python test suite                                      |
| `all`    | all of the above, in order (the default)                       |
| `clean`  | remove build artifacts and uninstall the package               |
| `help`   | usage                                                          |

```bash
./build.sh                # full pipeline
./build.sh native csmoke  # native library only, no Python in the loop
./build.sh help
```

If `./.venv` exists it is used automatically by every stage.

The `native` and `csmoke` stages deliberately never touch Python: when a number
looks wrong, `csmoke` vs `pytest` tells you immediately which side of the FFI
boundary the problem is on.

### Requirements

- C++20 compiler (tested on GCC 11.4)
- CMake ≥ 3.20 (pip-installed into the venv by `./build.sh venv`)
- Python ≥ 3.9

On Python 3.12+, uncomment `wheel.py-api = "cp312"` in `pyproject.toml` to produce
a single stable-ABI wheel covering all later CPython versions.

---

## Python API

```python
prepay.psa(speed=1.0, model_version=0)        # PSA ramp; 1.0 == 100 PSA
prepay.const_cpr(annual_cpr, model_version=0) # flat annual CPR in [0, 1]

prepay.project(model, pools, horizon)         # -> (n_pools, horizon) float64 SMM

prepay.smm_to_cpr(smm)                        # 1 - (1 - SMM)^12
prepay.cpr_to_smm(cpr)                        # 1 - (1 - CPR)^(1/12)

prepay.version()                              # runtime lib version
```

`pools` is any mapping or DataFrame with an `age` column (months). Optional
columns and their defaults:

| Field           | dtype     | Default | Meaning                        |
|-----------------|-----------|---------|--------------------------------|
| `age`           | `uint32`  | —       | loan age / WALA, in months     |
| `original_term` | `uint32`  | `360`   | amortization term, in months   |
| `balance`       | `float64` | `0.0`   | current outstanding balance    |
| `wac`           | `float64` | `0.0`   | weighted-average coupon        |
| `note_rate`     | `float64` | `0.0`   | note rate                      |

Everything is batch-first. One `project()` call crosses the FFI boundary exactly
once regardless of pool count, so pass the whole book rather than looping in
Python. The GIL is released during the native projection.

Errors from the C++ core surface as Python exceptions with the original detail
message:

```python
>>> prepay.const_cpr(1.5)
ValueError: CONST_CPR: annual CPR must be in [0, 1], got 1.500000
```

---

## C API

```c
#include <prepay/prepay.h>

prepay_model_config_t cfg = {0};
cfg.struct_size   = sizeof cfg;
cfg.model_version = 1;
cfg.type          = PREPAY_MODEL_PSA;
cfg.param         = 1.0;              /* 100 PSA */

prepay_model_t *m = NULL;
if (prepay_model_create(&cfg, &m) != PREPAY_OK) {
    fprintf(stderr, "%s\n", prepay_last_error());
    return 1;
}

prepay_pool_t pool = {0};
pool.struct_size = sizeof pool;
pool.age         = 0;

double smm[360];
prepay_status_t s = prepay_project(m, NULL, &pool, 1, 360, smm, 360);

prepay_model_destroy(m);
```

Conventions:

- Every call returns a `prepay_status_t`; `prepay_status_str()` gives the category
  and `prepay_last_error()` the specific detail.
- `prepay_last_error()` is **thread-local** — safe under a thread pool or numpy's
  released GIL. Valid until the next `prepay_*` call on the same thread.
- Always set `struct_size = sizeof(...)` on POD inputs. It is how the library
  detects ABI drift, and how fields get appended later without breaking you.
- Output is row-major: `out_smm[i * horizon + t]` for pool `i`, month `t`.
- Call `prepay_abi_compatible()` once after loading. It is `static inline` by
  design so it captures *your* compile-time version and compares it against the
  binary's runtime version.

---

## Model reference

**SMM** (single monthly mortality) is the per-period primitive the library returns;
it is what a cashflow engine consumes. **CPR** (conditional prepayment rate) is the
annualized display form:

SMM = 1 - (1 - CPR)^(1/12)
CPR = 1 - (1 - SMM)^12

### `CONST_CPR`

Flat annual CPR, converted once to SMM. Ignores age, rates, and scenario. Useful
as a baseline and for isolating cashflow-engine bugs from model bugs.

### `PSA`

The standard PSA ramp. At 100 PSA, CPR rises 0.2%/month to 6% at month 30, then
holds flat. `param` is the speed multiple (`1.0` == 100 PSA, `1.5` == 150 PSA).

**Seasoning convention:** output index `t` corresponds to the loan's
`(age + t + 1)`-th month of life, so a new pool (`age = 0`) begins at PSA month 1.
A pool with `age = 12` starts at 2.6% CPR. If your shop defines the ramp 0-based,
this is the one line to change (`src/model.cpp`, `psa_age`).

---

## Layout

libprepay/

├── CMakeLists.txt          # shared lib; gated python/ and tests/ subdirs

├── pyproject.toml          # scikit-build-core backend

├── build.sh                # staged build/test driver

├── include/prepay/

│   └── prepay.h            # THE public contract — the only shipped header

├── src/                    # private C++; never exposed as an interface

│   ├── prepay_abi.cpp      #   extern "C" seam: validation + exception translation

│   ├── prepay_error.cpp    #   thread-local last-error buffer

│   ├── prepay_version.cpp

│   ├── model.hpp/.cpp      #   Model interface + CONST_CPR/PSA + factory

│   ├── engine.hpp/.cpp     #   projection driver (row slicing)

│   └── error.hpp

├── python/

│   ├── CMakeLists.txt

│   ├── ext.cpp             # nanobind over the C ABI (not over the C++ core)

│   └── prepay/init.py  # numpy-native wrapper

└── tests/

├── smoke.c             # C-only ABI check

├── conftest.py

├── test_smoke.py

└── test_psa.py         # ramp/plateau/scaling property tests

The layering rule: `ext.cpp` binds the **C functions**, not the C++ classes. The
Python extension is a consumer of the same seam as everyone else, which is what
keeps the binding from quietly becoming the real interface.

---

## Versioning

Two independent versions, and the distinction matters:

- **Library / ABI version** (`PREPAY_VERSION`, `prepay_get_version()`) — tracks the
  binary interface. The `SOVERSION` follows the major, so `.so.0` bumps only on an
  ABI break.
- **Model version** (`prepay_model_config_t.model_version`) — caller-stamped and
  carried on every model. In a regulated setting, a number produced by model v3.2
  must be reproducible years later for audit. Old model versions are **frozen in
  place** rather than corrected; a behavior change means a new version, never an
  edit to an existing one.

---

## Testing

```bash
./build.sh csmoke     # C ABI, no Python involved
./build.sh pytest     # Python suite
```

- `tests/smoke.c` — exercises the raw ABI: create, project, destroy. Catches
  linkage, symbol visibility, and struct layout problems with nothing else in the
  way.
- `tests/test_psa.py` — property tests (ramp linearity, plateau, seasoning offset,
  linear scaling in speed, monotonicity, batch-equals-individual) rather than only
  golden values, so they stay meaningful as models gain components.
- `tests/test_smoke.py` — end-to-end values and error-translation checks.

---

## Roadmap

- [ ] `prepay_scenario_create/destroy` — rate paths and refi incentive
- [ ] Behavioral model: seasoning, turnover, refi S-curve, burnout, seasonality
- [ ] `prepay_cashflows()` — scheduled principal, prepaid principal, interest, balance
- [ ] Golden-master regression fixtures keyed by `model_version`
- [ ] Parallel projection (`std::execution::par` over the pool loop)
- [ ] Additional consumers of the C ABI: CFFI binding, Excel XLL

