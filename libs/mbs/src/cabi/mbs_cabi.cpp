/*
 * libmbs C ABI shims. The only translation unit that sees both the public
 * C header and the internal C++ core. Every exported function:
 *   - validates struct_size on input PODs
 *   - wraps the core call in try/catch and maps exceptions to mbs_errc
 *   - never lets an exception, nullptr deref, or C++ type reach the caller
 *
 * Every symbol declared in mbs.h is defined here, even the ones not yet
 * implemented (they return MBS_ERRC_UNSUPPORTED), so the .so always exports
 * the full 0.1 surface and a client never gets an unresolved symbol.
 */

#include "mbs/mbs.h"

#include "core/engine.hpp"
#include "arm/arm.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>

/* ---------------------------------------------------------------- types -- */

struct mbs_stream { mbs::core::Stream s; };

struct mbs_pool {
    mbs_pool_spec         spec;    /* normalised copy */
    mbs::core::Collateral col;
};

struct mbs_arm { 
	mbs_pool_spec           base; 
	mbs_arm_spec            arm; 
	mbs::core::Collateral   col;
	mbs::arm::ResetTerms    terms;
};
struct mbs_tba { mbs_tba_spec spec; };
struct mbs_cmo { int unused_; };

/* -------------------------------------------------------------- helpers -- */

namespace {

constexpr int32_t kVersionMajor = MBS_VERSION_MAJOR;
constexpr int32_t kVersionMinor = MBS_VERSION_MINOR;
constexpr int32_t kVersionPatch = MBS_VERSION_PATCH;

void set_err(mbs_err* err, int32_t code, const char* msg) {
    if (!err) return;
    err->code = code;
    if (err->struct_size >= sizeof(mbs_err)) {
        std::strncpy(err->message, msg, sizeof(err->message) - 1);
        err->message[sizeof(err->message) - 1] = '\0';
    }
}

/* Accept any struct_size we have ever shipped for this POD. For 0.1 there
 * is exactly one size per struct; later versions add to the allowed set. */
template <class T>
bool size_ok(const T* p) {
    return p && p->struct_size == sizeof(T);
}

/* Copy a POD into a caller buffer of elem_size bytes (output versioning). */
template <class T>
void copy_out(void* dst, size_t elem_size, const T& src) {
    std::memcpy(dst, &src, std::min(elem_size, sizeof(T)));
}

/* Translate core exceptions. Used as:  } catch (...) { return on_exception(err); } */
int32_t on_exception(mbs_err* err) {
    try { throw; }
    catch (const std::bad_alloc&)        { set_err(err, MBS_ERRC_OUT_OF_MEMORY, "out of memory"); return MBS_ERRC_OUT_OF_MEMORY; }
    catch (const std::invalid_argument& e){ set_err(err, MBS_ERRC_INVALID_ARG, e.what());         return MBS_ERRC_INVALID_ARG; }
    catch (const std::runtime_error& e)  {
        const std::string w = e.what();
        if (w.find("yield solver") != std::string::npos) { set_err(err, MBS_ERRC_NO_CONVERGENCE, e.what()); return MBS_ERRC_NO_CONVERGENCE; }
        if (w.find("prepay source") != std::string::npos) { set_err(err, MBS_ERRC_CALLBACK_FAILED, e.what()); return MBS_ERRC_CALLBACK_FAILED; }
        set_err(err, MBS_ERRC_INTERNAL, e.what()); return MBS_ERRC_INTERNAL;
    }
    catch (const std::exception& e)      { set_err(err, MBS_ERRC_INTERNAL, e.what());             return MBS_ERRC_INTERNAL; }
    catch (...)                          { set_err(err, MBS_ERRC_INTERNAL, "unknown exception");  return MBS_ERRC_INTERNAL; }
}

/* Carries a failing callback's code out of the core and into err. */
struct CallbackFailure { int32_t code; };

mbs::core::SpeedFn make_speed(const mbs_prepay_source* src) {
    return [src](const mbs::core::SpeedQuery& q) -> double {
        mbs_prepay_ctx ctx;
        ctx.struct_size    = sizeof ctx;
        ctx.period         = q.period;
        ctx.pay_date       = q.pay_date;
        ctx.wala           = q.wala;
        ctx.rem_term       = q.rem_term;
        ctx.wac            = q.wac;
        ctx.net_coupon     = q.net_coupon;
        ctx.current_coupon = q.current_coupon;
        ctx.begin_balance  = q.begin_balance;
        ctx.factor         = q.factor;
        ctx.market_rate    = q.market_rate;
        double smm = 0.0;
        const int32_t rc = src->fn(&ctx, src->user, &smm);
        if (rc != 0) throw CallbackFailure{ rc };
        return smm;
    };
}

std::vector<double> make_path(const mbs_rate_path* p) {
    if (!p || !p->rates || p->n == 0) return {};
    return std::vector<double>(p->rates, p->rates + p->n);
}

bool valid_source(const mbs_prepay_source* s) {
    return size_ok(s) && s->fn != nullptr;
}

mbs::arm::ResetTerms to_terms(const mbs_pool_spec& base, const mbs_arm_spec& a) {
    mbs::arm::ResetTerms t;
    t.margin                = a.margin;
    t.months_to_first_reset = a.months_to_first_reset;
    t.reset_freq_months     = a.reset_freq_months ? a.reset_freq_months : 12u;
    t.lookback_days         = a.lookback_days;
    t.initial_cap           = a.initial_cap;
    t.periodic_cap          = a.periodic_cap;
    t.lifetime_cap          = a.lifetime_cap;
    t.lifetime_floor        = a.lifetime_floor;
    t.rounding              = a.rounding;
    /* spread: explicit if given, else derived from the base pool's wac - net */
    t.servicing_spread      = a.net_margin_spread > 0 ? a.net_margin_spread : base.wac - base.net_coupon;
    return t;
}

mbs::core::Collateral to_collateral(const mbs_pool_spec& s) {
    mbs::core::Collateral c;
    c.orig_balance   = s.orig_balance;
    c.cur_balance    = s.current_balance;
    c.wac            = s.wac;
    c.net_coupon     = s.net_coupon;
    c.orig_term      = s.orig_term;
    c.wam            = s.wam;
    c.wala           = s.wala;
    c.pay_delay_days = s.pay_delay_days;
    c.next_pay_date  = s.next_pay_date;
    return c;
}

/* Validate and fill derived fields. Throws std::invalid_argument. */
mbs_pool_spec normalise(const mbs_pool_spec& in, bool alow_arm = false) {
    mbs_pool_spec s = in;
    if (s.product == MBS_PRODUCT_ARM) throw std::invalid_argument("product is ARM; use mbs_arm_new");
    if (!(s.wac > 0) || !(s.net_coupon >= 0) || s.net_coupon > s.wac)
        throw std::invalid_argument("need 0 <= net_coupon <= wac, wac > 0");
    if (s.wam == 0) throw std::invalid_argument("wam is required");
    if (s.orig_term == 0) s.orig_term = s.wam + s.wala;
    if (s.wam + s.wala > s.orig_term) throw std::invalid_argument("wam + wala exceeds orig_term");
    if (!(s.orig_balance > 0) && !(s.current_balance > 0))
        throw std::invalid_argument("orig_balance or current_balance is required");
    if (s.orig_balance <= 0) s.orig_balance = s.factor > 0 ? s.current_balance / s.factor : s.current_balance;
    if (s.current_balance <= 0) s.current_balance = s.factor > 0 ? s.orig_balance * s.factor : s.orig_balance;
    s.factor = s.current_balance / s.orig_balance;
    if (!mbs::core::valid_date(s.next_pay_date)) throw std::invalid_argument("next_pay_date is required (YYYYMMDD)");
    if (s.issue_date == 0) s.issue_date = mbs::core::add_months(s.next_pay_date, -static_cast<int>(s.wala) - 1);
    s.pool_id[sizeof s.pool_id - 1] = '\0';
    return s;
}

} // namespace

/* ------------------------------------------------------------- version --- */

extern "C" {

MBS_API void mbs_version(int32_t* major, int32_t* minor, int32_t* patch) {
    if (major) *major = kVersionMajor;
    if (minor) *minor = kVersionMinor;
    if (patch) *patch = kVersionPatch;
}

MBS_API const char* mbs_version_string(void) { return "0.1.0"; }

/* ------------------------------------------------------------- errors ---- */

MBS_API void mbs_err_init(mbs_err* err) {
    if (!err) return;
    err->struct_size = sizeof(mbs_err);
    err->code = 0;
    err->message[0] = '\0';
}

MBS_API const char* mbs_errc_name(int32_t code) {
    switch (code) {
        case MBS_ERRC_OK:                 return "MBS_ERRC_OK";
        case MBS_ERRC_INVALID_ARG:        return "MBS_ERRC_INVALID_ARG";
        case MBS_ERRC_BAD_STRUCT_SIZE:    return "MBS_ERRC_BAD_STRUCT_SIZE";
        case MBS_ERRC_OUT_OF_MEMORY:      return "MBS_ERRC_OUT_OF_MEMORY";
        case MBS_ERRC_BUFFER_TOO_SMALL:   return "MBS_ERRC_BUFFER_TOO_SMALL";
        case MBS_ERRC_INDEX_OUT_OF_RANGE: return "MBS_ERRC_INDEX_OUT_OF_RANGE";
        case MBS_ERRC_NO_CONVERGENCE:     return "MBS_ERRC_NO_CONVERGENCE";
        case MBS_ERRC_CALLBACK_FAILED:    return "MBS_ERRC_CALLBACK_FAILED";
        case MBS_ERRC_INVALID_STRUCTURE:  return "MBS_ERRC_INVALID_STRUCTURE";
        case MBS_ERRC_UNSUPPORTED:        return "MBS_ERRC_UNSUPPORTED";
        case MBS_ERRC_INTERNAL:           return "MBS_ERRC_INTERNAL";
        default:                          return "MBS_ERRC_?";
    }
}

/* ------------------------------------------------------------- prepay ---- */

MBS_API int32_t mbs_prepay_vector_fn(const mbs_prepay_ctx* ctx, void* user, double* smm_out) {
    const auto* v = static_cast<const mbs_prepay_vector*>(user);
    if (!ctx || !v || !smm_out || !size_ok(v) || !v->smm || v->n == 0) return 1;
    const size_t i = std::min<size_t>(ctx->period - 1, v->n - 1);
    *smm_out = v->smm[i];
    return 0;
}

MBS_API double mbs_cpr_to_smm(double cpr) { return mbs::core::cpr_to_smm(cpr); }
MBS_API double mbs_smm_to_cpr(double smm) { return mbs::core::smm_to_cpr(smm); }
MBS_API double mbs_psa_cpr(double psa, uint32_t wala) { return mbs::core::psa_cpr(psa, wala); }

/* ------------------------------------------------------------- stream ---- */

MBS_API void mbs_stream_free(mbs_stream_t* s) { delete s; }

MBS_API size_t mbs_stream_count(const mbs_stream_t* s) { return s ? s->s.periods.size() : 0; }

MBS_API double mbs_stream_orig_balance(const mbs_stream_t* s) { return s ? s->s.orig_balance : 0.0; }

MBS_API int32_t mbs_stream_copy(const mbs_stream_t* s,
                                mbs_cashflow* out, size_t cap, size_t elem_size,
                                size_t* n_written, mbs_err* err) {
    if (!s) { set_err(err, MBS_ERRC_INVALID_ARG, "stream is NULL"); return MBS_ERRC_INVALID_ARG; }
    const size_t n = s->s.periods.size();
    if (!out || cap == 0) {
        if (!n_written) { set_err(err, MBS_ERRC_INVALID_ARG, "n_written required for size query"); return MBS_ERRC_INVALID_ARG; }
        *n_written = n;
        return 0;
    }
    if (elem_size == 0) { set_err(err, MBS_ERRC_INVALID_ARG, "elem_size is 0"); return MBS_ERRC_INVALID_ARG; }
    const size_t m = std::min(cap, n);
    char* dst = reinterpret_cast<char*>(out);
    for (size_t i = 0; i < m; ++i, dst += elem_size) {
        const mbs::core::Period& p = s->s.periods[i];
        mbs_cashflow cf;
        std::memset(&cf, 0, sizeof cf);
        cf.period              = p.period;
        cf.pay_date            = p.pay_date;
        cf.begin_balance       = p.begin_balance;
        cf.scheduled_principal = p.scheduled_principal;
        cf.prepaid_principal   = p.prepaid_principal;
        cf.interest            = p.interest;
        cf.servicing           = p.servicing;
        cf.end_balance         = p.end_balance;
        cf.coupon              = p.coupon;
        cf.smm                 = p.smm;
        copy_out(dst, elem_size, cf);
    }
    if (n_written) *n_written = n;
    if (cap < n) { set_err(err, MBS_ERRC_BUFFER_TOO_SMALL, "cap < stream count"); return MBS_ERRC_BUFFER_TOO_SMALL; }
    return 0;
}

MBS_API int32_t mbs_stream_measures(const mbs_stream_t* s,
                                    const mbs_price_inputs* in,
                                    mbs_measures* out, size_t elem_size,
                                    mbs_err* err) {
    if (!s || !out || elem_size == 0) { set_err(err, MBS_ERRC_INVALID_ARG, "stream/out required"); return MBS_ERRC_INVALID_ARG; }
    if (!size_ok(in)) { set_err(err, MBS_ERRC_BAD_STRUCT_SIZE, "mbs_price_inputs.struct_size"); return MBS_ERRC_BAD_STRUCT_SIZE; }
    try {
        mbs::core::PriceInputs pi;
        pi.settle = in->settle;
        if (in->compounding == MBS_COMP_MONTHLY)         pi.comp = mbs::core::Compounding::Monthly;
        else if (in->compounding == MBS_COMP_SEMIANNUAL) pi.comp = mbs::core::Compounding::Semiannual;
        else throw std::invalid_argument("bad compounding");
        if (in->mode == MBS_PRICE_GIVEN)      pi.price_given = true;
        else if (in->mode == MBS_YIELD_GIVEN) pi.price_given = false;
        else throw std::invalid_argument("bad price mode");
        pi.price = in->price;
        pi.yield = in->yield;
        pi.pay_delay_days = in->pay_delay_days;

        const mbs::core::Measures m = mbs::core::measures(s->s, pi);
        mbs_measures o;
        o.price = m.price; o.yield = m.yield; o.accrued = m.accrued; o.wal = m.wal;
        o.mod_duration = m.mod_duration; o.convexity = m.convexity; o.dv01 = m.dv01;
        o.macaulay_duration = m.macaulay_duration;
        copy_out(out, elem_size, o);
        return 0;
    } catch (...) { return on_exception(err); }
}

/* ------------------------------------------------------------- pool ------ */

MBS_API mbs_pool_t* mbs_pool_new(const mbs_pool_spec* spec, mbs_err* err) {
    if (!spec) { set_err(err, MBS_ERRC_INVALID_ARG, "spec is NULL"); return nullptr; }
    if (!size_ok(spec)) { set_err(err, MBS_ERRC_BAD_STRUCT_SIZE, "mbs_pool_spec.struct_size"); return nullptr; }
    try {
        auto p = std::make_unique<mbs_pool>();
        p->spec = normalise(*spec);
        p->col  = to_collateral(p->spec);
        return p.release();
    } catch (...) { on_exception(err); return nullptr; }
}

MBS_API void mbs_pool_free(mbs_pool_t* p) { delete p; }

MBS_API int32_t mbs_pool_get_spec(const mbs_pool_t* p, mbs_pool_spec* out, mbs_err* err) {
    if (!p || !out) { set_err(err, MBS_ERRC_INVALID_ARG, "pool/out required"); return MBS_ERRC_INVALID_ARG; }
    if (out->struct_size == 0) { set_err(err, MBS_ERRC_BAD_STRUCT_SIZE, "out->struct_size"); return MBS_ERRC_BAD_STRUCT_SIZE; }
    const uint32_t keep = out->struct_size;
    copy_out(out, keep, p->spec);
    out->struct_size = keep;
    return 0;
}

MBS_API mbs_stream_t* mbs_pool_project(const mbs_pool_t* p,
                                       const mbs_prepay_source* prepay,
                                       const mbs_rate_path* path,
                                       mbs_err* err) {
    if (!p) { set_err(err, MBS_ERRC_INVALID_ARG, "pool is NULL"); return nullptr; }
    if (!valid_source(prepay)) { set_err(err, MBS_ERRC_INVALID_ARG, "prepay source invalid"); return nullptr; }
    if (path && !size_ok(path)) { set_err(err, MBS_ERRC_BAD_STRUCT_SIZE, "mbs_rate_path.struct_size"); return nullptr; }
    try {
        const double g = p->col.wac, n = p->col.net_coupon;
        auto s = std::make_unique<mbs_stream>();
        s->s = mbs::core::amortize(p->col, make_speed(prepay),
                                   [g, n](uint32_t, int32_t) { return mbs::core::CouponPair{ g, n }; },
                                   make_path(path));
        return s.release();
    }
    catch (const CallbackFailure& f) {
        set_err(err, MBS_ERRC_CALLBACK_FAILED, ("prepay callback returned " + std::to_string(f.code)).c_str());
        return nullptr;
    }
    catch (...) { on_exception(err); return nullptr; }
}

/* ------------------------------------------------------------- arm ------- */
/* Full surface exported; projection lands in 0.2. */

MBS_API mbs_arm_t* mbs_arm_new(const mbs_pool_spec* base, const mbs_arm_spec* arm, mbs_err* err) {
    if (!base || !arm) { set_err(err, MBS_ERRC_INVALID_ARG, "base/arm spec is NULL"); return nullptr; }
    if (!size_ok(base) || !size_ok(arm)) { set_err(err, MBS_ERRC_BAD_STRUCT_SIZE, "arm/base struct_size"); return nullptr; }
    if (base->product != MBS_PRODUCT_ARM) { set_err(err, MBS_ERRC_INVALID_ARG, "base->product must be MBS_PRODUCT_ARM"); return nullptr; }
    try {
        auto a = std::make_unique<mbs_arm>();
        a->base  = normalise(*base, /*allow_arm=*/true);
        a->arm   = *arm;
        a->col   = to_collateral(a->base);
        a->terms = to_terms(a->base, a->arm);
        mbs::arm::validate(a->terms);
        if (a->terms.servicing_spread > a->col.wac)
            throw std::invalid_argument("servicing spread exceeds initial coupon");
        return a.release();
    } catch (...) { on_exception(err); return nullptr; }
}

MBS_API void mbs_arm_free(mbs_arm_t* a) { delete a; }

MBS_API int32_t mbs_arm_get_spec(const mbs_arm_t* a, mbs_pool_spec* base_out, mbs_arm_spec* arm_out, mbs_err* err) {
    if (!a) { set_err(err, MBS_ERRC_INVALID_ARG, "arm is NULL"); return MBS_ERRC_INVALID_ARG; }
    if (base_out && base_out->struct_size) { uint32_t k = base_out->struct_size; copy_out(base_out, k, a->base); base_out->struct_size = k; }
    if (arm_out  && arm_out->struct_size)  { uint32_t k = arm_out->struct_size;  copy_out(arm_out,  k, a->arm);  arm_out->struct_size  = k; }
    return 0;
}

/* path is the index path and is required. It doubles as the market rate seen
 * by the prepay callback (ctx->market_rate) in 0.1; a separate market path
 * is reserved for a later minor version. */
MBS_API mbs_stream_t* mbs_arm_project(const mbs_arm_t* a,
                                      const mbs_prepay_source* prepay,
                                      const mbs_rate_path* path,
                                      mbs_err* err) {
    if (!a) { set_err(err, MBS_ERRC_INVALID_ARG, "arm is NULL"); return nullptr; }
    if (!valid_source(prepay)) { set_err(err, MBS_ERRC_INVALID_ARG, "prepay source invalid"); return nullptr; }
    if (!path) { set_err(err, MBS_ERRC_INVALID_ARG, "index path is required for ARM"); return nullptr; }
    if (!size_ok(path)) { set_err(err, MBS_ERRC_BAD_STRUCT_SIZE, "mbs_rate_path.struct_size"); return nullptr; }
    if (!path->rates || path->n == 0) { set_err(err, MBS_ERRC_INVALID_ARG, "index path is empty"); return nullptr; }
    try {
        auto s = std::make_unique<mbs_stream>();
        s->s = mbs::arm::project(a->col, a->terms, make_path(path), make_speed(prepay));
        return s.release();
    }
    catch (const CallbackFailure& f) {
        set_err(err, MBS_ERRC_CALLBACK_FAILED, ("prepay callback returned " + std::to_string(f.code)).c_str());
        return nullptr;
    }
    catch (...) { on_exception(err); return nullptr; }
}

/* ------------------------------------------------------------- tba ------- */

MBS_API mbs_tba_t* mbs_tba_new(const mbs_tba_spec* spec, mbs_err* err) {
    if (!size_ok(spec)) { set_err(err, MBS_ERRC_BAD_STRUCT_SIZE, "mbs_tba_spec.struct_size"); return nullptr; }
    try { auto t = std::make_unique<mbs_tba>(); t->spec = *spec; return t.release(); }
    catch (...) { on_exception(err); return nullptr; }
}
MBS_API void mbs_tba_free(mbs_tba_t* t) { delete t; }
MBS_API int32_t mbs_tba_get_spec(const mbs_tba_t* t, mbs_tba_spec* out, mbs_err* err) {
    if (!t || !out || !out->struct_size) { set_err(err, MBS_ERRC_INVALID_ARG, "tba/out required"); return MBS_ERRC_INVALID_ARG; }
    uint32_t k = out->struct_size; copy_out(out, k, t->spec); out->struct_size = k; return 0;
}
MBS_API int32_t mbs_tba_settle_date(const mbs_tba_t*, mbs_date*, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "mbs_tba_settle_date: 0.2"); return MBS_ERRC_UNSUPPORTED;
}
MBS_API int32_t mbs_tba_generic_spec(const mbs_tba_t*, mbs_pool_spec*, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "mbs_tba_generic_spec: 0.2"); return MBS_ERRC_UNSUPPORTED;
}
MBS_API mbs_pool_t* mbs_tba_generic_pool(const mbs_tba_t*, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "mbs_tba_generic_pool: 0.2"); return nullptr;
}

/* ------------------------------------------------------------- cmo ------- */

MBS_API mbs_cmo_t* mbs_cmo_new(const mbs_pool_t* const*, size_t, const mbs_tranche_spec*, size_t, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "mbs_cmo_new: 0.3"); return nullptr;
}
MBS_API void mbs_cmo_free(mbs_cmo_t* c) { delete c; }
MBS_API size_t mbs_cmo_tranche_count(const mbs_cmo_t*) { return 0; }
MBS_API int32_t mbs_cmo_tranche_spec(const mbs_cmo_t*, size_t, mbs_tranche_spec*, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "0.3"); return MBS_ERRC_UNSUPPORTED;
}
MBS_API int32_t mbs_cmo_tranche_index(const mbs_cmo_t*, const char*) { return -1; }
MBS_API int32_t mbs_cmo_pac_schedule(const mbs_cmo_t*, size_t, double*, size_t, size_t*, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "0.3"); return MBS_ERRC_UNSUPPORTED;
}
MBS_API int32_t mbs_cmo_project(const mbs_cmo_t*, const mbs_prepay_source*, const mbs_rate_path*,
                                mbs_stream_t**, size_t, size_t*, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "0.3"); return MBS_ERRC_UNSUPPORTED;
}
MBS_API mbs_stream_t* mbs_cmo_collateral_project(const mbs_cmo_t*, const mbs_prepay_source*, const mbs_rate_path*, mbs_err* err) {
    set_err(err, MBS_ERRC_UNSUPPORTED, "0.3"); return nullptr;
}

} /* extern "C" */
