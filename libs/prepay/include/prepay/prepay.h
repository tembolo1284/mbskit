#ifndef PREPAY_H_INCLUDED
#define PREPAY_H_INCLUDED

/*
 * prepay - mortgage prepayment model library
 * Public C ABI. This is the *only* header consumers (Python/nanobind, an XLL,
 * another language) are meant to include. Everything behind it is private C++.
 *
 * Design rules for this file:
 *   - Flat C ABI: opaque handles + versioned POD structs, no C++ types exposed.
 *   - No exceptions cross this boundary; every call returns a prepay_status_t.
 *   - Light includes, suppressible for binding/preprocessor tooling.
 *   - Stable ABI: append-only structs (first member is struct_size), add new
 *     functions rather than changing existing signatures.
 */

/* ---- includes (suppressible for CFFI-style preprocessing) -------------- */
#ifndef PREPAY_NO_INCLUDES
#  include <stddef.h>   /* size_t   */
#  include <stdint.h>   /* uint32_t */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- export / visibility markers --------------------------------------- */
/* When building the shared lib, define PREPAY_BUILD_SHARED. Consumers that do
 * nothing get dllimport on Windows and default visibility elsewhere. */
#ifndef PREPAY_API
#  ifdef _WIN32
#    if defined(PREPAY_BUILD_SHARED)
#      define PREPAY_API __declspec(dllexport)
#    elif !defined(PREPAY_BUILD_STATIC)
#      define PREPAY_API __declspec(dllimport)
#    else
#      define PREPAY_API
#    endif
#  else
#    if defined(__GNUC__) && (__GNUC__ >= 4)
#      define PREPAY_API __attribute__((visibility("default")))
#    else
#      define PREPAY_API
#    endif
#  endif
#endif

/* ---- version ----------------------------------------------------------- */
/* Encoded as (major << 16) | (minor << 8) | patch. The header carries these
 * constants; the binary returns prepay_get_version(). Compare to catch a
 * header/binary mismatch early.
 *
 * 0.2.0: scenario lifecycle, PREPAY_MODEL_REFI_SCURVE, appended S-curve
 *        parameters, audit metadata getters. Append-only: a 0.1.0 caller
 *        links and runs against a 0.2.0 binary unchanged. */
#define PREPAY_VERSION_MAJOR 0
#define PREPAY_VERSION_MINOR 2
#define PREPAY_VERSION_PATCH 0
#define PREPAY_VERSION \
    ((PREPAY_VERSION_MAJOR << 16) | (PREPAY_VERSION_MINOR << 8) | PREPAY_VERSION_PATCH)

/* Version compiled into the binary. */
PREPAY_API uint32_t prepay_get_version(void);

/* Nonzero if the linked binary is ABI-compatible with the header THIS caller
 * compiled against (same major). Defined inline on purpose: it captures the
 * caller's PREPAY_VERSION_MAJOR at the caller's compile time and compares it to
 * the binary's runtime prepay_get_version(). Call once after loading. */
static inline int prepay_abi_compatible(void) {
    return (int)((prepay_get_version() >> 16) & 0xFFu) == PREPAY_VERSION_MAJOR;
}

/* ---- status codes ------------------------------------------------------ */
typedef enum {
    PREPAY_OK              = 0,
    PREPAY_ERR_NULL_ARG    = 1,  /* a required pointer was NULL                */
    PREPAY_ERR_INVALID_ARG = 2,  /* a value was out of range / struct_size bad */
    PREPAY_ERR_BUFFER_SIZE = 3,  /* out_len != n_pools * horizon               */
    PREPAY_ERR_UNSUPPORTED = 4,  /* e.g. model needs a scenario but got NULL   */
    PREPAY_ERR_ALLOC       = 5,  /* allocation failed                          */
    PREPAY_ERR_INTERNAL    = 6,  /* caught C++ exception / unexpected state    */
    PREPAY_ERR_SHORT_PATH  = 7   /* scenario rate path shorter than horizon    */
} prepay_status_t;

/* Static, never-NULL string for a status code (safe across the ABI). */
PREPAY_API const char *prepay_status_str(prepay_status_t status);

/* Thread-local, human-readable detail for the most recent failing call on the
 * current thread. Valid until the next prepay_* call on that thread. */
PREPAY_API const char *prepay_last_error(void);

/* ---- opaque handles ---------------------------------------------------- */
typedef struct prepay_model    prepay_model_t;
typedef struct prepay_scenario prepay_scenario_t;  /* rate/economic environment */

/* ---- model configuration (versioned POD) ------------------------------- */
typedef enum {
    PREPAY_MODEL_CONST_CPR   = 0,  /* flat annual CPR, ignores age and rates   */
    PREPAY_MODEL_PSA         = 1,  /* PSA ramp: CPR ramps 0.2%/mo to a plateau */
    PREPAY_MODEL_REFI_SCURVE = 2   /* logistic in refi incentive; needs a scenario */
} prepay_model_type_t;

typedef struct {
    uint32_t            struct_size;   /* set to sizeof(prepay_model_config_t) */
    uint32_t            model_version; /* caller-stamped, for audit/reproducibility */
    prepay_model_type_t type;
    double              param;         /* CONST_CPR: annual CPR in [0,1]
                                          PSA: speed multiple (1.0 == 100 PSA)
                                          REFI_SCURVE: unused */

    /* --- appended in 0.2.0: REFI_SCURVE parameters, ignored by other models ---
     * Annual CPR follows
     *     cpr(t) = season(t) * [floor + (ceiling - floor) * L(incentive(t))]
     *     L(x)   = 1 / (1 + exp(-steepness * (x - midpoint)))
     * where incentive(t) = pool.wac - scenario.mortgage_rate[t], in annual
     * decimal (0.01 == 100bp), and season(t) ramps linearly from 1/seasoning
     * to 1.0 over the pool's first `seasoning_months` months of life.
     *
     * A 0.1.0 caller passes struct_size == 24 and never sets these; the library
     * sees the short struct and only accepts CONST_CPR / PSA from it. */
    double scurve_floor;            /* turnover-driven baseline CPR, e.g. 0.05  */
    double scurve_ceiling;          /* max CPR deep in-the-money, e.g. 0.45     */
    double scurve_midpoint;         /* incentive at half height, e.g. 0.0075    */
    double scurve_steepness;        /* logistic slope; ~400 for decimal units   */
    double scurve_seasoning_months; /* ramp length in months; <= 1 disables it  */
} prepay_model_config_t;

/* ---- pool descriptor (versioned POD, one row per pool) ----------------- */
/* Carries more than CONST_CPR/PSA need today (wac, note_rate, balance) so the
 * row layout is already meaningful for behavioral models. struct_size lets us
 * append fields without breaking existing callers. */
typedef struct {
    uint32_t struct_size;    /* set to sizeof(prepay_pool_t)            */
    uint32_t original_term;  /* amortization term in months, e.g. 360   */
    uint32_t age;            /* loan age (WALA) in months               */
    double   balance;        /* current outstanding balance             */
    double   wac;            /* weighted-average coupon, annual decimal */
    double   note_rate;      /* note rate, annual decimal               */
} prepay_pool_t;

/* ---- scenario configuration (versioned POD) ---------------------------- */
/* A scenario is the rate environment a rate-sensitive model reads. The library
 * COPIES mortgage_rate on create, so the caller may free its buffer as soon as
 * prepay_scenario_create returns. n_months must be >= the horizon passed to
 * prepay_project, or projection returns PREPAY_ERR_SHORT_PATH. */
typedef struct {
    uint32_t      struct_size;   /* set to sizeof(prepay_scenario_config_t)      */
    uint32_t      scenario_id;   /* caller-stamped, echoed by prepay_scenario_id */
    const double *mortgage_rate; /* monthly par mortgage rate, annual decimal    */
    size_t        n_months;      /* length of mortgage_rate                      */
} prepay_scenario_config_t;

/* ---- lifecycle --------------------------------------------------------- */
/* On success writes an owned handle to *out_model (free with
 * prepay_model_destroy). On failure returns a status and leaves *out_model
 * unchanged; see prepay_last_error(). */
PREPAY_API prepay_status_t prepay_model_create(const prepay_model_config_t *cfg,
                                               prepay_model_t **out_model);

/* NULL-safe. */
PREPAY_API void prepay_model_destroy(prepay_model_t *model);

/* Same contract as prepay_model_create: owned handle out, unchanged on failure. */
PREPAY_API prepay_status_t prepay_scenario_create(const prepay_scenario_config_t *cfg,
                                                  prepay_scenario_t **out_scenario);

/* NULL-safe. */
PREPAY_API void prepay_scenario_destroy(prepay_scenario_t *scenario);

/* ---- audit metadata ---------------------------------------------------- */
/* Echo back the caller-stamped stamps, so a result set can be traced to the
 * exact model config and rate path that produced it. */
PREPAY_API prepay_status_t prepay_model_version(const prepay_model_t *model,
                                                uint32_t *out_version);

PREPAY_API prepay_status_t prepay_scenario_id(const prepay_scenario_t *scenario,
                                              uint32_t *out_id);

/* ---- projection (batch workhorse) -------------------------------------- */
/* Projects single monthly mortality (SMM) for each pool over `horizon` months.
 * Output is row-major:  out_smm[i * horizon + t]  for pool i, month t in
 * [0, horizon). out_len must equal n_pools * horizon.
 *
 * SMM is the per-period primitive the cashflow engine consumes;
 * CPR = 1 - (1 - SMM)^12 for display.
 *
 * `scenario` may be NULL for rate-independent models (CONST_CPR, PSA); passing
 * NULL to a model that requires it returns PREPAY_ERR_UNSUPPORTED, and passing
 * one whose path is shorter than `horizon` returns PREPAY_ERR_SHORT_PATH. The
 * whole call crosses the FFI boundary once — keep batches large. */
PREPAY_API prepay_status_t prepay_project(const prepay_model_t    *model,
                                          const prepay_scenario_t *scenario,
                                          const prepay_pool_t     *pools,
                                          size_t                   n_pools,
                                          size_t                   horizon,
                                          double                  *out_smm,
                                          size_t                   out_len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* PREPAY_H_INCLUDED */
