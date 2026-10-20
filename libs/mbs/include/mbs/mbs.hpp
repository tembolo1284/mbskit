#ifndef MBS_MBS_HPP
#define MBS_MBS_HPP

/*
 * libmbs C++ wrapper -- the top of the hourglass.
 *
 * Header-only, built purely on mbs.h. Restores what the waist gave up:
 *   - RAII handles (unique_ptr + the matching _free)
 *   - exceptions (mbs::Error carries the mbs_errc and message)
 *   - value types (std::vector<CashFlow>, std::string)
 *   - callbacks as std::function, with exceptions carried across the waist
 *
 * Nothing here is ABI. A client may replace this file wholesale and keep
 * linking the same libmbs.so.
 */

#include "mbs/mbs.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mbs {

/* ------------------------------------------------------------- errors ---- */

class Error : public std::runtime_error {
public:
    Error(int32_t code, const std::string& msg)
        : std::runtime_error(std::string(mbs_errc_name(code)) + ": " + msg), code_(code) {}
    int32_t code() const noexcept { return code_; }
private:
    int32_t code_;
};

namespace detail {

struct Err {
    mbs_err e;
    Err() { mbs_err_init(&e); }
    mbs_err* ptr() { return &e; }
    [[noreturn]] void raise() const { throw Error(e.code, e.message); }
    void check(int32_t rc) const { if (rc != 0) raise(); }
};

template <class T>
T zeroed() { T t; std::memset(&t, 0, sizeof t); t.struct_size = sizeof t; return t; }

template <class T, void (*F)(T*)>
struct Deleter { void operator()(T* p) const noexcept { F(p); } };

} // namespace detail

/* ------------------------------------------------------------- PODs ------ */
/* The specs are the C structs; make<T>() zero-fills and sets struct_size so
 * designated-style fill-in is safe. */

using PoolSpec    = mbs_pool_spec;
using ArmSpec     = mbs_arm_spec;
using TbaSpec     = mbs_tba_spec;
using TrancheSpec = mbs_tranche_spec;
using CashFlow    = mbs_cashflow;
using Measures    = mbs_measures;
using PrepayCtx   = mbs_prepay_ctx;
using Date        = mbs_date;

template <class T> T make() { return detail::zeroed<T>(); }

inline std::pair<int32_t, int32_t> version() {
    int32_t a = 0, b = 0, c = 0;
    mbs_version(&a, &b, &c);
    return { a, b };
}
inline std::string versionString() { return mbs_version_string(); }

inline double cprToSmm(double cpr) { return mbs_cpr_to_smm(cpr); }
inline double smmToCpr(double smm) { return mbs_smm_to_cpr(smm); }
inline double psaCpr(double psa, uint32_t wala) { return mbs_psa_cpr(psa, wala); }

/* ------------------------------------------------------------- RatePath -- */

class RatePath {
public:
    RatePath() = default;
    explicit RatePath(std::vector<double> rates) : rates_(std::move(rates)) {}

    /* Borrowed view for the call; valid while *this lives. */
    mbs_rate_path view() const {
        mbs_rate_path p;
        p.struct_size = sizeof p;
        p.rates = rates_.empty() ? nullptr : rates_.data();
        p.n = rates_.size();
        return p;
    }
    bool empty() const { return rates_.empty(); }
private:
    std::vector<double> rates_;
};

/* ------------------------------------------------------------- Prepay ---- */

/* A prepayment model as a std::function. The trampoline catches anything the
 * function throws, parks it, returns non-zero to the waist, and the owning
 * project() rethrows it once the C call has unwound -- so an exception from
 * user code crosses the C boundary without ever *being* a C++ exception
 * while it is on the C side of the stack. */
class PrepaySource {
public:
    using Fn = std::function<double(const PrepayCtx&)>;

    explicit PrepaySource(Fn fn) : fn_(std::move(fn)) {}

    static PrepaySource constantSmm(double smm) {
        return PrepaySource([smm](const PrepayCtx&) { return smm; });
    }
    static PrepaySource constantCpr(double cpr) { return constantSmm(cprToSmm(cpr)); }
    static PrepaySource psa(double multiple) {
        return PrepaySource([multiple](const PrepayCtx& c) { return cprToSmm(psaCpr(multiple, c.wala)); });
    }
    /* Period i -> smm[i-1], last value held (mirrors mbs_prepay_vector_fn). */
    static PrepaySource vector(std::vector<double> smm) {
        auto v = std::make_shared<std::vector<double>>(std::move(smm));
        return PrepaySource([v](const PrepayCtx& c) {
            if (v->empty()) return 0.0;
            const size_t i = c.period - 1 < v->size() ? c.period - 1 : v->size() - 1;
            return (*v)[i];
        });
    }

    /* Borrowed descriptor for the duration of one C call. */
    mbs_prepay_source view() const {
        pending_ = nullptr;
        mbs_prepay_source s;
        s.struct_size = sizeof s;
        s.fn   = &PrepaySource::trampoline;
        s.user = const_cast<PrepaySource*>(this);
        return s;
    }
    /* Called by the wrapper after a failed C call. */
    void rethrowIfPending() const {
        if (pending_) { std::exception_ptr p = pending_; pending_ = nullptr; std::rethrow_exception(p); }
    }

private:
    static int32_t trampoline(const mbs_prepay_ctx* ctx, void* user, double* out) noexcept {
        auto* self = static_cast<PrepaySource*>(user);
        try { *out = self->fn_(*ctx); return 0; }
        catch (...) { self->pending_ = std::current_exception(); return 1; }
    }
    Fn fn_;
    mutable std::exception_ptr pending_;
};

/* ------------------------------------------------------------- Stream ---- */

enum class Compounding : int32_t { Monthly = MBS_COMP_MONTHLY, Semiannual = MBS_COMP_SEMIANNUAL };

class Stream {
public:
    using Handle = std::unique_ptr<mbs_stream_t, detail::Deleter<mbs_stream_t, mbs_stream_free>>;

    explicit Stream(mbs_stream_t* h) : h_(h) {}

    size_t size() const { return mbs_stream_count(h_.get()); }
    double origBalance() const { return mbs_stream_orig_balance(h_.get()); }

    std::vector<CashFlow> cashflows() const {
        detail::Err e;
        size_t n = 0;
        e.check(mbs_stream_copy(h_.get(), nullptr, 0, 0, &n, e.ptr()));
        std::vector<CashFlow> out(n);
        if (n) e.check(mbs_stream_copy(h_.get(), out.data(), n, sizeof(CashFlow), &n, e.ptr()));
        return out;
    }

    Measures measures(const mbs_price_inputs& in) const {
        detail::Err e;
        Measures m;
        e.check(mbs_stream_measures(h_.get(), &in, &m, sizeof m, e.ptr()));
        return m;
    }
    Measures priceFromYield(Date settle, double y, Compounding c = Compounding::Semiannual) const {
        auto in = make<mbs_price_inputs>();
        in.mode = MBS_YIELD_GIVEN; in.settle = settle; in.yield = y;
        in.compounding = static_cast<int32_t>(c);
        return measures(in);
    }
    Measures yieldFromPrice(Date settle, double price, Compounding c = Compounding::Semiannual) const {
        auto in = make<mbs_price_inputs>();
        in.mode = MBS_PRICE_GIVEN; in.settle = settle; in.price = price;
        in.compounding = static_cast<int32_t>(c);
        return measures(in);
    }

    mbs_stream_t* get() const { return h_.get(); }
private:
    Handle h_;
};

/* ------------------------------------------------------------- Pool ------ */

class Pool {
public:
    using Handle = std::unique_ptr<mbs_pool_t, detail::Deleter<mbs_pool_t, mbs_pool_free>>;

    explicit Pool(const PoolSpec& spec) {
        detail::Err e;
        mbs_pool_t* h = mbs_pool_new(&spec, e.ptr());
        if (!h) e.raise();
        h_.reset(h);
    }

    PoolSpec spec() const {
        detail::Err e;
        auto s = make<PoolSpec>();
        e.check(mbs_pool_get_spec(h_.get(), &s, e.ptr()));
        return s;
    }

    Stream project(const PrepaySource& prepay, const RatePath& path = RatePath()) const {
        detail::Err e;
        const mbs_prepay_source src = prepay.view();
        const mbs_rate_path rp = path.view();
        mbs_stream_t* s = mbs_pool_project(h_.get(), &src, path.empty() ? nullptr : &rp, e.ptr());
        if (!s) { prepay.rethrowIfPending(); e.raise(); }
        return Stream(s);
    }

    mbs_pool_t* get() const { return h_.get(); }
private:
    Handle h_;
};

/* ------------------------------------------------------------- Arm ------- */

class Arm {
public:
    using Handle = std::unique_ptr<mbs_arm_t, detail::Deleter<mbs_arm_t, mbs_arm_free>>;

    Arm(const PoolSpec& base, const ArmSpec& arm) {
        detail::Err e;
        mbs_arm_t* h = mbs_arm_new(&base, &arm, e.ptr());
        if (!h) e.raise();
        h_.reset(h);
    }
    std::pair<PoolSpec, ArmSpec> spec() const {
        detail::Err e;
        auto b = make<PoolSpec>(); auto a = make<ArmSpec>();
        e.check(mbs_arm_get_spec(h_.get(), &b, &a, e.ptr()));
        return { b, a };
    }
    Stream project(const PrepaySource& prepay, const RatePath& indexPath) const {
        detail::Err e;
        const mbs_prepay_source src = prepay.view();
        const mbs_rate_path rp = indexPath.view();
        mbs_stream_t* s = mbs_arm_project(h_.get(), &src, &rp, e.ptr());
        if (!s) { prepay.rethrowIfPending(); e.raise(); }
        return Stream(s);
    }
    mbs_arm_t* get() const { return h_.get(); }
private:
    Handle h_;
};

/* ------------------------------------------------------------- Tba ------- */

class Tba {
public:
    using Handle = std::unique_ptr<mbs_tba_t, detail::Deleter<mbs_tba_t, mbs_tba_free>>;

    explicit Tba(const TbaSpec& spec) {
        detail::Err e;
        mbs_tba_t* h = mbs_tba_new(&spec, e.ptr());
        if (!h) e.raise();
        h_.reset(h);
    }
    TbaSpec spec() const {
        detail::Err e; auto s = make<TbaSpec>();
        e.check(mbs_tba_get_spec(h_.get(), &s, e.ptr())); return s;
    }
    Date settleDate() const {
        detail::Err e; Date d = 0;
        e.check(mbs_tba_settle_date(h_.get(), &d, e.ptr())); return d;
    }
    PoolSpec genericSpec() const {
        detail::Err e; auto s = make<PoolSpec>();
        e.check(mbs_tba_generic_spec(h_.get(), &s, e.ptr())); return s;
    }
    Pool genericPool() const { return Pool(genericSpec()); }
    mbs_tba_t* get() const { return h_.get(); }
private:
    Handle h_;
};

/* ------------------------------------------------------------- Cmo ------- */

class Cmo;

/* Non-owning view of one tranche. This is the wrapper-level convenience the
 * waist deliberately does not offer: it holds a reference to the Cmo and an
 * index, and every accessor goes back through an indexed C call. */
class TrancheView {
public:
    TrancheView(const Cmo& cmo, size_t idx) : cmo_(&cmo), idx_(idx) {}
    TrancheSpec spec() const;
    std::string name() const { auto s = spec(); return std::string(s.name); }
    std::vector<double> pacSchedule() const;
    size_t index() const { return idx_; }
private:
    const Cmo* cmo_;
    size_t     idx_;
};

class Cmo {
public:
    using Handle = std::unique_ptr<mbs_cmo_t, detail::Deleter<mbs_cmo_t, mbs_cmo_free>>;

    Cmo(const std::vector<const Pool*>& collateral, const std::vector<TrancheSpec>& tranches) {
        detail::Err e;
        std::vector<const mbs_pool_t*> raw;
        raw.reserve(collateral.size());
        for (const Pool* p : collateral) raw.push_back(p->get());
        mbs_cmo_t* h = mbs_cmo_new(raw.data(), raw.size(), tranches.data(), tranches.size(), e.ptr());
        if (!h) e.raise();
        h_.reset(h);
    }

    size_t trancheCount() const { return mbs_cmo_tranche_count(h_.get()); }
    TrancheView tranche(size_t idx) const { return TrancheView(*this, idx); }
    TrancheView tranche(const std::string& name) const {
        const int32_t i = mbs_cmo_tranche_index(h_.get(), name.c_str());
        if (i < 0) throw Error(MBS_ERRC_INDEX_OUT_OF_RANGE, "no tranche named " + name);
        return TrancheView(*this, static_cast<size_t>(i));
    }

    /* One Stream per tranche, deal order. */
    std::vector<Stream> project(const PrepaySource& prepay, const RatePath& path = RatePath()) const {
        detail::Err e;
        const size_t n = trancheCount();
        std::vector<mbs_stream_t*> raw(n, nullptr);
        const mbs_prepay_source src = prepay.view();
        const mbs_rate_path rp = path.view();
        size_t written = 0;
        const int32_t rc = mbs_cmo_project(h_.get(), &src, path.empty() ? nullptr : &rp,
                                           raw.data(), raw.size(), &written, e.ptr());
        if (rc != 0) { prepay.rethrowIfPending(); e.raise(); }
        std::vector<Stream> out;
        out.reserve(written);
        for (size_t i = 0; i < written; ++i) out.emplace_back(raw[i]);
        return out;
    }
    Stream collateral(const PrepaySource& prepay, const RatePath& path = RatePath()) const {
        detail::Err e;
        const mbs_prepay_source src = prepay.view();
        const mbs_rate_path rp = path.view();
        mbs_stream_t* s = mbs_cmo_collateral_project(h_.get(), &src, path.empty() ? nullptr : &rp, e.ptr());
        if (!s) { prepay.rethrowIfPending(); e.raise(); }
        return Stream(s);
    }

    mbs_cmo_t* get() const { return h_.get(); }
private:
    Handle h_;
};

inline TrancheSpec TrancheView::spec() const {
    detail::Err e; auto s = make<TrancheSpec>();
    e.check(mbs_cmo_tranche_spec(cmo_->get(), idx_, &s, e.ptr())); return s;
}
inline std::vector<double> TrancheView::pacSchedule() const {
    detail::Err e; size_t n = 0;
    e.check(mbs_cmo_pac_schedule(cmo_->get(), idx_, nullptr, 0, &n, e.ptr()));
    std::vector<double> out(n);
    if (n) e.check(mbs_cmo_pac_schedule(cmo_->get(), idx_, out.data(), n, &n, e.ptr()));
    return out;
}

} // namespace mbs

#endif /* MBS_MBS_HPP */
