#ifndef MBS_ARM_ARM_HPP
#define MBS_ARM_ARM_HPP

/*
 * ARM coupon policy. Plain C++17, waist-unaware.
 *
 * Produces a core::CouponFn for core::amortize(): the gross coupon in force
 * in each period, given an index path, margin, reset schedule, caps/floors,
 * lookback and rounding. The net coupon is gross minus a fixed servicing
 * spread, so it floats with the gross.
 *
 * Reset mechanics (standard agency hybrid/annual-reset ARM):
 *   - Periods 1 .. months_to_first_reset-1 pay the current (teaser) coupon.
 *   - At the first reset and every reset_freq_months after, the fully
 *     indexed rate is index + margin, where index is observed `lookback`
 *     *periods* before the reset (lookback_days / 30, rounded).
 *   - Rounded to the nearest `rounding` increment if non-zero.
 *   - Then capped: |new - old| <= initial_cap (first) / periodic_cap (later)
 *     when the cap is > 0; and lifetime_floor <= new <= lifetime_cap when set.
 *   - Between resets the coupon is held.
 */

#include "core/engine.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mbs::arm {

struct ResetTerms {
    double   margin                = 0;
    uint32_t months_to_first_reset = 0;    /* 0 => already floating: reset at period 1 */
    uint32_t reset_freq_months     = 12;
    uint32_t lookback_days         = 45;
    double   initial_cap           = 0;    /* 0 => none */
    double   periodic_cap          = 0;
    double   lifetime_cap          = 0;    /* 0 => none */
    double   lifetime_floor        = 0;    /* applied as-is (0 is a valid floor) */
    double   rounding              = 0;    /* e.g. 0.00125; 0 => none */
    double   servicing_spread      = 0;    /* gross - net, carried through resets */
};

inline void validate(const ResetTerms& t) {
    if (t.reset_freq_months == 0) throw std::invalid_argument("arm: reset_freq_months must be > 0");
    if (!(t.margin >= 0))         throw std::invalid_argument("arm: margin must be >= 0");
    if (t.initial_cap < 0 || t.periodic_cap < 0 || t.lifetime_cap < 0 || t.rounding < 0)
        throw std::invalid_argument("arm: caps and rounding must be >= 0");
    if (t.lifetime_cap > 0 && t.lifetime_floor > t.lifetime_cap)
        throw std::invalid_argument("arm: lifetime_floor exceeds lifetime_cap");
    if (!(t.servicing_spread >= 0)) throw std::invalid_argument("arm: servicing_spread must be >= 0");
}

inline double round_to(double x, double inc) {
    if (inc <= 0) return x;
    return std::round(x / inc) * inc;
}

inline bool is_reset_period(const ResetTerms& t, uint32_t period /*1-based*/) {
    const uint32_t first = t.months_to_first_reset == 0 ? 1 : t.months_to_first_reset;
    if (period < first) return false;
    return (period - first) % t.reset_freq_months == 0;
}

/* Index observed for a reset at `period`: lookback_days/30 periods earlier,
 * clamped to period 1. Last path value is held past the end. */
inline double observed_index(const std::vector<double>& index_path,
                             const ResetTerms& t, uint32_t period) {
    const uint32_t lag = (t.lookback_days + 15u) / 30u;
    const uint32_t obs = period > lag ? period - lag : 1u;
    return core::path_at(index_path, obs);
}

/* Stateful coupon policy. One instance per projection; not reentrant. */
class CouponPolicy {
public:
    CouponPolicy(double initial_gross, ResetTerms terms, std::vector<double> index_path)
        : cur_(initial_gross), terms_(terms), path_(std::move(index_path)), reset_count_(0) {
        validate(terms_);
        if (path_.empty()) throw std::invalid_argument("arm: index path is required");
        if (!(initial_gross > 0)) throw std::invalid_argument("arm: initial gross coupon must be > 0");
        if (terms_.servicing_spread > initial_gross)
            throw std::invalid_argument("arm: servicing_spread exceeds initial coupon");
    }

    core::CouponPair operator()(uint32_t period, int32_t /*pay_date*/) {
        if (is_reset_period(terms_, period)) {
            const double idx = observed_index(path_, terms_, period);
            if (std::isnan(idx)) throw std::invalid_argument("arm: index path has no value for reset");
            double next = round_to(idx + terms_.margin, terms_.rounding);

            const double cap = reset_count_ == 0 ? terms_.initial_cap : terms_.periodic_cap;
            if (cap > 0) {
                next = std::min(next, cur_ + cap);
                next = std::max(next, cur_ - cap);
            }
            if (terms_.lifetime_cap > 0) next = std::min(next, terms_.lifetime_cap);
            next = std::max(next, terms_.lifetime_floor);
            /* net can never go negative: floor gross at the servicing spread */
            next = std::max(next, terms_.servicing_spread);

            cur_ = next;
            ++reset_count_;
        }
        return { cur_, cur_ - terms_.servicing_spread };
    }

    double current() const { return cur_; }

private:
    double              cur_;
    ResetTerms          terms_;
    std::vector<double> path_;
    uint32_t            reset_count_;
};

/* Convenience: project an ARM. Collateral.wac is the current (teaser) gross
 * coupon; Collateral.net_coupon is ignored in favour of terms.servicing_spread. */
inline core::Stream project(const core::Collateral& c,
                            const ResetTerms& terms,
                            const std::vector<double>& index_path,
                            const core::SpeedFn& speed,
                            const std::vector<double>& market_path = {}) {
    CouponPolicy policy(c.wac, terms, index_path);
    core::CouponFn fn = [&policy](uint32_t k, int32_t d) { return policy(k, d); };
    return core::amortize(c, speed, fn, market_path.empty() ? index_path : market_path);
}

} // namespace mbs::arm

#endif /* MBS_ARM_ARM_HPP */
