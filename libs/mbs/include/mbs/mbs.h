#ifndef MBS_MBS_H
#define MBS_MBS_H

/*
 * mbskit / libmbs -- agency MBS cash-flow and risk engine.
 *
 * C ABI waist. See docs/cabi-conventions.md for the rules this header obeys.
 * Compiles as C99 and C++17. Depends on nothing but <stddef.h>, <stdint.h>.
 *
 * Object model
 *   pool    : specified pool, fixed rate               (mbs_pool_t)
 *   arm     : specified pool with a reset schedule     (mbs_arm_t)
 *   tba     : to-be-announced forward on a generic pool (mbs_tba_t)
 *   cmo     : structure of tranches over pool collateral (mbs_cmo_t)
 *   stream  : materialised period-by-period cash flows (mbs_stream_t)
 *
 * pool/arm/tba/cmo are projected into streams by supplying a prepayment
 * source (a callback) and, where needed, a market rate path. Streams are
 * then priced, yielded and measured.
 */

#include <stddef.h>
#include <stdint.h>

#include "mbs/mbs_export.h"   /* defines MBS_API */

#define MBS_VERSION_MAJOR 0
#define MBS_VERSION_MINOR 1
#define MBS_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Version                                                                  */
/* ------------------------------------------------------------------------ */

/* Runtime library version. Any out-pointer may be NULL. */
MBS_API void        mbs_version(int32_t* major, int32_t* minor, int32_t* patch);
/* Static string, e.g. "0.1.0". Never NULL, never freed. */
MBS_API const char* mbs_version_string(void);

/* ------------------------------------------------------------------------ */
/* Errors                                                                   */
/* ------------------------------------------------------------------------ */

typedef enum mbs_errc {
    MBS_ERRC_OK                 = 0,
    MBS_ERRC_INVALID_ARG        = 1,   /* NULL where required, bad enum, NaN, negative balance */
    MBS_ERRC_BAD_STRUCT_SIZE    = 2,   /* struct_size not one this library has ever shipped   */
    MBS_ERRC_OUT_OF_MEMORY      = 3,
    MBS_ERRC_BUFFER_TOO_SMALL   = 4,   /* two-call pattern: required count still reported     */
    MBS_ERRC_INDEX_OUT_OF_RANGE = 5,
    MBS_ERRC_NO_CONVERGENCE     = 6,   /* yield solver                                         */
    MBS_ERRC_CALLBACK_FAILED    = 7,   /* a user callback returned non-zero                    */
    MBS_ERRC_INVALID_STRUCTURE  = 8,   /* cmo: tranche balances != collateral, bad priorities */
    MBS_ERRC_UNSUPPORTED        = 9,   /* valid request this version does not implement        */
    MBS_ERRC_INTERNAL           = 100, /* uncaught C++ exception in the shim                   */
    MBS_ERRC_FORCE_INT32        = 0x7fffffff
} mbs_errc;

/* Stack-allocatable, no free. Initialise once with MBS_ERR_INIT or mbs_err_init.
 * On failure the library sets code and writes a NUL-terminated message.
 * On success it is untouched. */
typedef struct mbs_err {
    uint32_t struct_size;
    int32_t  code;              /* mbs_errc */
    char     message[248];
} mbs_err;

#define MBS_ERR_INIT { (uint32_t)sizeof(mbs_err), 0, { 0 } }

MBS_API void        mbs_err_init(mbs_err* err);        /* NULL ok */
MBS_API const char* mbs_errc_name(int32_t code);       /* static, e.g. "MBS_ERRC_INVALID_ARG" */

/* ------------------------------------------------------------------------ */
/* Scalars and enums                                                        */
/* ------------------------------------------------------------------------ */

/* Dates are int32_t YYYYMMDD. 0 means "unset". */
typedef int32_t mbs_date;

typedef enum mbs_agency {
    MBS_AGENCY_UNSPECIFIED = 0,
    MBS_AGENCY_FNMA        = 1,
    MBS_AGENCY_FHLMC       = 2,
    MBS_AGENCY_GNMA1       = 3,
    MBS_AGENCY_GNMA2       = 4,
    MBS_AGENCY_UMBS        = 5,
    MBS_AGENCY_FORCE_INT32 = 0x7fffffff
} mbs_agency;

typedef enum mbs_product {
    MBS_PRODUCT_UNSPECIFIED = 0,
    MBS_PRODUCT_FIXED_30    = 1,
    MBS_PRODUCT_FIXED_20    = 2,
    MBS_PRODUCT_FIXED_15    = 3,
    MBS_PRODUCT_FIXED_10    = 4,
    MBS_PRODUCT_ARM         = 10,
    MBS_PRODUCT_FORCE_INT32 = 0x7fffffff
} mbs_product;

typedef enum mbs_daycount {
    MBS_DC_30_360       = 0,     /* agency pass-through convention */
    MBS_DC_ACT_360      = 1,
    MBS_DC_ACT_365      = 2,
    MBS_DC_ACT_ACT      = 3,
    MBS_DC_FORCE_INT32  = 0x7fffffff
} mbs_daycount;

typedef enum mbs_rate_index {
    MBS_INDEX_UNSPECIFIED = 0,
    MBS_INDEX_SOFR_30D    = 1,
    MBS_INDEX_CMT_1Y      = 2,
    MBS_INDEX_CMT_5Y      = 3,
    MBS_INDEX_FORCE_INT32 = 0x7fffffff
} mbs_rate_index;

typedef enum mbs_compounding {
    MBS_COMP_MONTHLY      = 0,   /* mortgage yield, 12x */
    MBS_COMP_SEMIANNUAL   = 1,   /* bond-equivalent yield */
    MBS_COMP_FORCE_INT32  = 0x7fffffff
} mbs_compounding;

/* ------------------------------------------------------------------------ */
/* Rate paths                                                               */
/* ------------------------------------------------------------------------ */

/* A per-period vector of rates (decimals), index 0 = first projected period.
 * Borrowed for the duration of the call. If a projection needs more periods
 * than n, the last value is held. n == 0 with rates == NULL means "no path"
 * (market_rate in the prepay context is then NAN). */
typedef struct mbs_rate_path {
    uint32_t      struct_size;
    const double* rates;
    size_t        n;
} mbs_rate_path;

/* ------------------------------------------------------------------------ */
/* Prepayment source (callback)                                             */
/* ------------------------------------------------------------------------ */

/* What the engine knows about the collateral at the moment it asks for a
 * prepayment speed. Filled by the library; read-only to the callback.
 * Fields are only ever appended. */
typedef struct mbs_prepay_ctx {
    uint32_t struct_size;
    uint32_t period;          /* 1-based projection period                   */
    mbs_date pay_date;
    uint32_t wala;            /* months, as of this period                   */
    uint32_t rem_term;        /* months remaining incl. this one             */
    double   wac;             /* gross coupon, decimal                       */
    double   net_coupon;      /* pass-through coupon, decimal                */
    double   current_coupon;  /* == wac for fixed; reset rate for ARM        */
    double   begin_balance;
    double   factor;          /* begin_balance / orig_balance                */
    double   market_rate;     /* from mbs_rate_path, or NAN                  */
} mbs_prepay_ctx;

/* Return 0 and write the single-month mortality (decimal, 0 <= smm < 1) for
 * this period. Non-zero aborts the projection with MBS_ERRC_CALLBACK_FAILED.
 * Must not throw, must not call back into the handle being projected. */
typedef int32_t (*mbs_prepay_fn)(const mbs_prepay_ctx* ctx, void* user, double* smm_out);

typedef struct mbs_prepay_source {
    uint32_t      struct_size;
    mbs_prepay_fn fn;
    void*         user;       /* passed through untouched */
} mbs_prepay_source;

/* Canned source: a borrowed SMM vector, period i -> smm[i-1], last value held.
 * Usage:
 *   mbs_prepay_vector v = { sizeof v, smm, n };
 *   mbs_prepay_source s = { sizeof s, mbs_prepay_vector_fn, &v };
 * The vector must outlive the projection call. */
typedef struct mbs_prepay_vector {
    uint32_t      struct_size;
    const double* smm;
    size_t        n;
} mbs_prepay_vector;

MBS_API int32_t mbs_prepay_vector_fn(const mbs_prepay_ctx* ctx, void* user, double* smm_out);

/* Helpers so clients don't re-derive the identities. */
MBS_API double mbs_cpr_to_smm(double cpr);
MBS_API double mbs_smm_to_cpr(double smm);
/* PSA ramp: CPR for a given WALA at a given PSA multiple (1.0 == 100 PSA). */
MBS_API double mbs_psa_cpr(double psa, uint32_t wala);

/* ------------------------------------------------------------------------ */
/* Cash flows and streams                                                   */
/* ------------------------------------------------------------------------ */

/* One period of a projected stream. Output POD: versioned by the elem_size
 * argument of the function that fills it, so no struct_size here. */
typedef struct mbs_cashflow {
    uint32_t period;              /* 1-based                                  */
    uint32_t pad_;                /* keep doubles 8-aligned; always 0         */
    mbs_date pay_date;
    int32_t  pad2_;
    double   begin_balance;
    double   scheduled_principal;
    double   prepaid_principal;
    double   interest;            /* to the holder (net)                      */
    double   servicing;           /* wac - net_coupon portion                 */
    double   end_balance;
    double   coupon;              /* net coupon in force this period          */
    double   smm;                 /* speed used this period                   */
} mbs_cashflow;

typedef struct mbs_stream mbs_stream_t;

MBS_API void   mbs_stream_free(mbs_stream_t* s);                 /* NULL ok */
MBS_API size_t mbs_stream_count(const mbs_stream_t* s);          /* 0 if NULL */
MBS_API double mbs_stream_orig_balance(const mbs_stream_t* s);

/* Two-call: out == NULL / cap == 0 -> *n_written = count required, returns 0.
 * Otherwise copies min(cap, count) records of elem_size bytes each; if
 * cap < count returns MBS_ERRC_BUFFER_TOO_SMALL and still sets *n_written
 * to the required count. n_written may be NULL only if out != NULL. */
MBS_API int32_t mbs_stream_copy(const mbs_stream_t* s,
                                mbs_cashflow* out, size_t cap, size_t elem_size,
                                size_t* n_written, mbs_err* err);

/* Pricing inputs. Exactly one of price / yield is the independent variable. */
typedef enum mbs_price_mode {
    MBS_PRICE_GIVEN       = 0,   /* solve yield from price */
    MBS_YIELD_GIVEN       = 1,   /* compute price from yield */
    MBS_PRICE_FORCE_INT32 = 0x7fffffff
} mbs_price_mode;

typedef struct mbs_price_inputs {
    uint32_t struct_size;
    int32_t  mode;               /* mbs_price_mode                          */
    mbs_date settle;
    int32_t  compounding;        /* mbs_compounding of the yield            */
    double   price;              /* per 100 par, clean                      */
    double   yield;              /* decimal                                 */
    uint32_t pay_delay_days;     /* 0 => use the stream's own delay         */
    uint32_t pad_;
} mbs_price_inputs;

/* Output POD; versioned by elem_size. All per-100 or decimal as noted. */
typedef struct mbs_measures {
    double price;                /* clean, per 100                          */
    double yield;                /* in the requested compounding            */
    double accrued;              /* per 100                                 */
    double wal;                  /* years, from settle                      */
    double mod_duration;         /* years                                   */
    double convexity;
    double dv01;                 /* per 100 par per 1bp                     */
    double macaulay_duration;
} mbs_measures;

MBS_API int32_t mbs_stream_measures(const mbs_stream_t* s,
                                    const mbs_price_inputs* in,
                                    mbs_measures* out, size_t elem_size,
                                    mbs_err* err);

/* ------------------------------------------------------------------------ */
/* Specified pool (fixed rate)                                              */
/* ------------------------------------------------------------------------ */

typedef struct mbs_pool_spec {
    uint32_t struct_size;
    int32_t  agency;             /* mbs_agency                              */
    int32_t  product;            /* mbs_product; MBS_PRODUCT_ARM => use mbs_arm_new */
    int32_t  daycount;           /* mbs_daycount                            */
    double   orig_balance;
    double   current_balance;    /* 0 => orig_balance * factor; both 0 => orig */
    double   factor;             /* 0 => derived from balances              */
    double   wac;                /* gross, decimal                          */
    double   net_coupon;         /* pass-through, decimal                   */
    uint32_t orig_term;          /* months                                  */
    uint32_t wam;                /* months                                  */
    uint32_t wala;               /* months                                  */
    uint32_t pay_delay_days;     /* 24 (FNMA/FHLMC), 44 (GNMA I), 19 (GNMA II) etc. */
    mbs_date issue_date;
    mbs_date next_pay_date;      /* first projected payment                 */
    char     pool_id[16];        /* informational, NUL-terminated           */
} mbs_pool_spec;

typedef struct mbs_pool mbs_pool_t;

/* Validates and copies the spec. NULL on failure. */
MBS_API mbs_pool_t* mbs_pool_new(const mbs_pool_spec* spec, mbs_err* err);
MBS_API void        mbs_pool_free(mbs_pool_t* p);                   /* NULL ok */

/* Copies back the normalised spec (derived fields filled in). out->struct_size
 * must be set by the caller. */
MBS_API int32_t     mbs_pool_get_spec(const mbs_pool_t* p, mbs_pool_spec* out, mbs_err* err);

/* Project to a new stream the caller owns. path may be NULL. */
MBS_API mbs_stream_t* mbs_pool_project(const mbs_pool_t* p,
                                       const mbs_prepay_source* prepay,
                                       const mbs_rate_path* path,
                                       mbs_err* err);

/* ------------------------------------------------------------------------ */
/* ARM                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct mbs_arm_spec {
    uint32_t struct_size;
    int32_t  index;                  /* mbs_rate_index                      */
    double   margin;                 /* decimal                             */
    uint32_t months_to_first_reset;  /* from next_pay_date                  */
    uint32_t reset_freq_months;      /* 12 for annual                       */
    uint32_t lookback_days;          /* index observation lag               */
    uint32_t pad_;
    double   initial_cap;            /* abs change at first reset; 0 => none */
    double   periodic_cap;           /* abs change per subsequent reset     */
    double   lifetime_cap;           /* absolute rate ceiling               */
    double   lifetime_floor;         /* absolute rate floor                 */
    double   rounding;               /* e.g. 0.00125; 0 => none             */
    double   net_margin_spread;      /* wac - net_coupon carried through resets */
} mbs_arm_spec;

typedef struct mbs_arm mbs_arm_t;

/* base->product must be MBS_PRODUCT_ARM. Both specs are copied. */
MBS_API mbs_arm_t* mbs_arm_new(const mbs_pool_spec* base, const mbs_arm_spec* arm, mbs_err* err);
MBS_API void       mbs_arm_free(mbs_arm_t* a);
MBS_API int32_t    mbs_arm_get_spec(const mbs_arm_t* a, mbs_pool_spec* base_out,
                                    mbs_arm_spec* arm_out, mbs_err* err);

/* path is the index path and is required (NULL => MBS_ERRC_INVALID_ARG).
 * The reset coupon for a period is derived from path at (period - lookback). */
MBS_API mbs_stream_t* mbs_arm_project(const mbs_arm_t* a,
                                      const mbs_prepay_source* prepay,
                                      const mbs_rate_path* path,
                                      mbs_err* err);

/* ------------------------------------------------------------------------ */
/* TBA                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct mbs_tba_spec {
    uint32_t struct_size;
    int32_t  agency;             /* mbs_agency                              */
    int32_t  product;            /* fixed products only                     */
    int32_t  pad_;
    double   coupon;             /* pass-through coupon, decimal            */
    mbs_date settle_month;       /* YYYYMM01; settlement day is derived     */
    int32_t  pad2_;
    double   price;              /* per 100, optional (0 => unset)          */
    double   generic_wac_spread; /* wac = coupon + this; 0 => library default */
    uint32_t generic_wala;       /* 0 => library default for the coupon/product */
    uint32_t pad3_;
} mbs_tba_spec;

typedef struct mbs_tba mbs_tba_t;

MBS_API mbs_tba_t* mbs_tba_new(const mbs_tba_spec* spec, mbs_err* err);
MBS_API void       mbs_tba_free(mbs_tba_t* t);
MBS_API int32_t    mbs_tba_get_spec(const mbs_tba_t* t, mbs_tba_spec* out, mbs_err* err);

/* SIFMA settlement date for the class/month. */
MBS_API int32_t    mbs_tba_settle_date(const mbs_tba_t* t, mbs_date* out, mbs_err* err);

/* The cheapest-to-deliver proxy as a pool spec (out->struct_size set by caller). */
MBS_API int32_t    mbs_tba_generic_spec(const mbs_tba_t* t, mbs_pool_spec* out, mbs_err* err);

/* Convenience: construct the generic pool. Caller owns the result. */
MBS_API mbs_pool_t* mbs_tba_generic_pool(const mbs_tba_t* t, mbs_err* err);

/* Reserved for a later minor version: dollar roll breakeven drop.
 * MBS_API int32_t mbs_tba_roll_breakeven(const mbs_tba_t* front, const mbs_tba_t* back,
 *                                        const mbs_prepay_source*, double repo_rate,
 *                                        double* drop_out, mbs_err*); */

/* ------------------------------------------------------------------------ */
/* CMO                                                                      */
/* ------------------------------------------------------------------------ */

typedef enum mbs_tranche_kind {
    MBS_TRANCHE_UNSPECIFIED = 0,
    MBS_TRANCHE_SEQUENTIAL  = 1,
    MBS_TRANCHE_PAC         = 2,
    MBS_TRANCHE_SUPPORT     = 3,   /* companion */
    MBS_TRANCHE_Z           = 4,   /* accrual */
    MBS_TRANCHE_IO          = 5,
    MBS_TRANCHE_PO          = 6,
    MBS_TRANCHE_FLOATER     = 7,
    MBS_TRANCHE_INVERSE     = 8,
    MBS_TRANCHE_FORCE_INT32 = 0x7fffffff
} mbs_tranche_kind;

typedef struct mbs_tranche_spec {
    uint32_t struct_size;
    int32_t  kind;               /* mbs_tranche_kind                        */
    char     name[16];           /* NUL-terminated, unique within the deal  */
    double   orig_balance;       /* notional for IO                         */
    double   coupon;             /* decimal; floater: margin over index     */
    uint32_t priority;           /* principal order, 0 = first; ties split pro rata */
    int32_t  accrual_target;     /* Z: index of tranche receiving accrued interest; -1 => sequential */
    double   pac_low_psa;        /* PAC band, multiples (1.0 == 100 PSA)    */
    double   pac_high_psa;
    double   cap;                /* floater/inverse; 0 => none              */
    double   floor;
    double   leverage;           /* inverse: coupon = (cap - leverage*index), floored */
    int32_t  index;              /* mbs_rate_index for floater/inverse      */
    int32_t  pad_;
} mbs_tranche_spec;

typedef struct mbs_cmo mbs_cmo_t;

/* Collateral pools and tranche specs are copied; the caller may free the
 * pools immediately. Sum of non-IO tranche balances must equal collateral
 * current balance within 0.01 or MBS_ERRC_INVALID_STRUCTURE. */
MBS_API mbs_cmo_t* mbs_cmo_new(const mbs_pool_t* const* collateral, size_t n_pools,
                               const mbs_tranche_spec* tranches, size_t n_tranches,
                               mbs_err* err);
MBS_API void       mbs_cmo_free(mbs_cmo_t* c);

MBS_API size_t     mbs_cmo_tranche_count(const mbs_cmo_t* c);
MBS_API int32_t    mbs_cmo_tranche_spec(const mbs_cmo_t* c, size_t idx,
                                        mbs_tranche_spec* out, mbs_err* err);
/* -1 if not found. */
MBS_API int32_t    mbs_cmo_tranche_index(const mbs_cmo_t* c, const char* name);

/* PAC principal schedule for tranche idx, one entry per collateral period.
 * Two-call pattern. Non-PAC tranche => MBS_ERRC_INVALID_ARG. */
MBS_API int32_t    mbs_cmo_pac_schedule(const mbs_cmo_t* c, size_t idx,
                                        double* out, size_t cap, size_t* n_written,
                                        mbs_err* err);

/* Runs the waterfall once and returns one owned stream per tranche in deal
 * order. cap must be >= tranche_count or MBS_ERRC_BUFFER_TOO_SMALL (and
 * *n_written = tranche_count, no streams allocated). On success the caller
 * frees each stream with mbs_stream_free. path may be NULL if no tranche
 * or collateral needs an index. */
MBS_API int32_t    mbs_cmo_project(const mbs_cmo_t* c,
                                   const mbs_prepay_source* prepay,
                                   const mbs_rate_path* path,
                                   mbs_stream_t** out, size_t cap, size_t* n_written,
                                   mbs_err* err);

/* Aggregate collateral stream (what the waterfall consumed). Caller owns. */
MBS_API mbs_stream_t* mbs_cmo_collateral_project(const mbs_cmo_t* c,
                                                 const mbs_prepay_source* prepay,
                                                 const mbs_rate_path* path,
                                                 mbs_err* err);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MBS_MBS_H */
