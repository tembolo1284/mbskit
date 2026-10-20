#ifndef MBS_CORE_ENGINE_HPP
#define MBS_CORE_ENGINE_HPP

/*
 * libmbs core: amortisation engine, dates, pricing. Plain C++17.
 * This file knows nothing about the C ABI. Only src/cabi/ may include it
 * together with <mbs/mbs.h>.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mbs::core {

/* ---------------------------------------------------------------- dates -- */

struct Ymd { int y, m, d; };

inline Ymd split_yyyymmdd(int32_t v) {
    return { v / 10000, (v / 100) % 100, v % 100 };
}
inline int32_t join_yyyymmdd(Ymd x) {
    return x.y * 10000 + x.m * 100 + x.d;
}

/* Howard Hinnant's days-from-civil, so date arithmetic is exact. */
inline int64_t days_from_civil(Ymd x) {
    int y = x.y - (x.m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(x.m + (x.m > 2 ? -3 : 9)) + 2) / 5
                       + static_cast<unsigned>(x.d) - 1u;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<int64_t>(doe) - 719468;
}
inline Ymd civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return { y + (m <= 2), static_cast<int>(m), static_cast<int>(d) };
}
inline int days_in_month(int y, int m) {
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}
inline int32_t add_months(int32_t yyyymmdd, int n) {
    Ymd x = split_yyyymmdd(yyyymmdd);
    int total = x.y * 12 + (x.m - 1) + n;
    x.y = total / 12;
    x.m = total % 12 + 1;
    x.d = std::min(x.d, days_in_month(x.y, x.m));
    return join_yyyymmdd(x);
}
inline int32_t add_days(int32_t yyyymmdd, int n) {
    return join_yyyymmdd(civil_from_days(days_from_civil(split_yyyymmdd(yyyymmdd)) + n));
}
inline int64_t day_diff(int32_t a, int32_t b) {   /* b - a in days */
    return days_from_civil(split_yyyymmdd(b)) - days_from_civil(split_yyyymmdd(a));
}
inline bool valid_date(int32_t v) {
    if (v <= 0) return false;
    Ymd x = split_yyyymmdd(v);
    return x.y >= 1900 && x.m >= 1 && x.m <= 12 && x.d >= 1 && x.d <= days_in_month(x.y, x.m);
}

/* ---------------------------------------------------------- speeds ------ */

inline double cpr_to_smm(double cpr) { return 1.0 - std::pow(1.0 - cpr, 1.0 / 12.0); }
inline double smm_to_cpr(double smm) { return 1.0 - std::pow(1.0 - smm, 12.0); }
inline double psa_cpr(double psa, uint32_t wala) {
    const double ramp = wala >= 30 ? 0.06 : 0.002 * static_cast<double>(wala);
    return psa * ramp;
}

/* ---------------------------------------------------------- collateral -- */

struct Collateral {
    double   orig_balance   = 0;
    double   cur_balance    = 0;
    double   wac            = 0;      /* gross, decimal */
    double   net_coupon     = 0;      /* pass-through, decimal */
    uint32_t orig_term      = 0;
    uint32_t wam            = 0;
    uint32_t wala           = 0;
    uint32_t pay_delay_days = 0;
    int32_t  next_pay_date  = 0;      /* yyyymmdd */
};

/* What the engine tells a prepay model each period. */
struct SpeedQuery {
    uint32_t period;
    int32_t  pay_date;
    uint32_t wala;
    uint32_t rem_term;
    double   wac;
    double   net_coupon;
    double   current_coupon;
    double   begin_balance;
    double   factor;
    double   market_rate;     /* NAN when no path */
};

/* One projected period. Mirrors the ABI record but is our own type. */
struct Period {
    uint32_t period;
    int32_t  pay_date;
    double   begin_balance;
    double   scheduled_principal;
    double   prepaid_principal;
    double   interest;
    double   servicing;
    double   end_balance;
    double   coupon;          /* net coupon in force */
    double   smm;
};

struct Stream {
    double              orig_balance = 0;
    uint32_t            pay_delay_days = 0;
    std::vector<Period> periods;
};

/* Market rate path: index 0 = first projected period; last value held. */
inline double path_at(const std::vector<double>& p, uint32_t period /*1-based*/) {
    if (p.empty()) return std::nan("");
    const size_t i = std::min<size_t>(period - 1, p.size() - 1);
    return p[i];
}

/* Coupon policy: given the period, return (gross, net) in force.
 * Fixed pools return the same pair every time; ARMs reset. */
struct CouponPair { double gross; double net; };
using CouponFn = std::function<CouponPair(uint32_t period, int32_t pay_date)>;
using SpeedFn  = std::function<double(const SpeedQuery&)>;

/* Level-pay amortiser. Payment is recomputed each period from the coupon in
 * force and the remaining term, which makes ARM resets fall out for free.
 * Throws std::invalid_argument on bad inputs and std::runtime_error if the
 * speed function returns something outside [0,1). */
inline Stream amortize(const Collateral& c,
                       const SpeedFn& speed,
                       const CouponFn& coupon,
                       const std::vector<double>& market_path = {}) {
    if (c.cur_balance <= 0 || c.wam == 0 || !valid_date(c.next_pay_date))
        throw std::invalid_argument("amortize: balance, wam and next_pay_date are required");

    Stream s;
    s.orig_balance   = c.orig_balance > 0 ? c.orig_balance : c.cur_balance;
    s.pay_delay_days = c.pay_delay_days;
    s.periods.reserve(c.wam);

    double bal = c.cur_balance;
    for (uint32_t k = 1; k <= c.wam && bal > 1e-9; ++k) {
        const int32_t  pay_date = add_months(c.next_pay_date, static_cast<int>(k - 1));
        const uint32_t rem      = c.wam - (k - 1);
        const CouponPair cp     = coupon(k, pay_date);
        const double r          = cp.gross / 12.0;

        double payment;
        if (r > 0) {
            payment = bal * r / (1.0 - std::pow(1.0 + r, -static_cast<double>(rem)));
        } else {
            payment = bal / static_cast<double>(rem);
        }
        double sched = payment - bal * r;
        sched = std::min(sched, bal);

        SpeedQuery q{ k, pay_date, c.wala + (k - 1), rem, c.wac, cp.net, cp.gross,
                      bal, bal / s.orig_balance, path_at(market_path, k) };
        const double smm = speed(q);
        if (!(smm >= 0.0 && smm < 1.0))
            throw std::runtime_error("prepay source returned SMM outside [0,1)");

        const double prepay = (bal - sched) * smm;
        Period p;
        p.period              = k;
        p.pay_date            = pay_date;
        p.begin_balance       = bal;
        p.scheduled_principal = sched;
        p.prepaid_principal   = prepay;
        p.interest            = bal * cp.net / 12.0;
        p.servicing           = bal * (cp.gross - cp.net) / 12.0;
        p.coupon              = cp.net;
        p.smm                 = smm;
        bal                   = bal - sched - prepay;
        p.end_balance         = bal < 1e-9 ? 0.0 : bal;
        s.periods.push_back(p);
    }
    return s;
}

/* ---------------------------------------------------------- pricing ----- */

enum class Compounding { Monthly, Semiannual };

struct PriceInputs {
    int32_t     settle;
    Compounding comp;
    double      price = 0;      /* per 100 clean */
    double      yield = 0;      /* decimal */
    bool        price_given;
    uint32_t    pay_delay_days = 0;   /* 0 => stream's */
};

struct Measures {
    double price, yield, accrued, wal, mod_duration, convexity, dv01, macaulay_duration;
};

inline double monthly_equiv(double y, Compounding c) {
    return c == Compounding::Monthly ? y / 12.0 : std::pow(1.0 + y / 2.0, 1.0 / 6.0) - 1.0;
}
inline double from_monthly(double ym, Compounding c) {
    return c == Compounding::Monthly ? ym * 12.0 : 2.0 * (std::pow(1.0 + ym, 6.0) - 1.0);
}

/* Accrued interest per 100 of the balance outstanding at settle:
 * 30/360 from the 1st of the settle month. */
inline double accrued_per_100(const Stream& s, int32_t settle) {
    if (s.periods.empty()) return 0.0;
    const Ymd sd = split_yyyymmdd(settle);
    const double days = static_cast<double>(std::min(sd.d, 30) - 1);
    return 100.0 * s.periods.front().coupon / 12.0 * days / 30.0;
}

/* Dirty PV per 100 of settle balance at a monthly yield, with payment delay:
 * cash flow k is received on pay_date_k + delay, discounted by the actual
 * day count from settle expressed in months. */
inline double pv_per_100(const Stream& s, int32_t settle, double ym, uint32_t delay) {
    if (s.periods.empty()) return 0.0;
    const double bal0 = s.periods.front().begin_balance;
    double pv = 0.0;
    for (const Period& p : s.periods) {
        const int32_t recv = add_days(p.pay_date, static_cast<int>(delay));
        const double t = static_cast<double>(day_diff(settle, recv)) / (365.25 / 12.0);
        if (t < 0) continue;   /* flows before settle are not ours */
        const double cf = p.scheduled_principal + p.prepaid_principal + p.interest;
        pv += cf / std::pow(1.0 + ym, t);
    }
    return 100.0 * pv / bal0;
}

inline double clean_from_yield(const Stream& s, int32_t settle, double ym, uint32_t delay) {
    return pv_per_100(s, settle, ym, delay) - accrued_per_100(s, settle);
}

inline double solve_monthly_yield(const Stream& s, int32_t settle, double clean, uint32_t delay) {
    double lo = -0.05 / 12.0, hi = 0.50 / 12.0;
    double flo = clean_from_yield(s, settle, lo, delay) - clean;
    double fhi = clean_from_yield(s, settle, hi, delay) - clean;
    if (flo * fhi > 0) throw std::runtime_error("yield solver: no bracket");
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double fm = clean_from_yield(s, settle, mid, delay) - clean;
        if (std::fabs(fm) < 1e-10 || (hi - lo) < 1e-14) return mid;
        if (fm * flo < 0) { hi = mid; fhi = fm; } else { lo = mid; flo = fm; }
    }
    throw std::runtime_error("yield solver: no convergence");
}

inline Measures measures(const Stream& s, const PriceInputs& in) {
    if (!valid_date(in.settle)) throw std::invalid_argument("measures: bad settle date");
    const uint32_t delay = in.pay_delay_days ? in.pay_delay_days : s.pay_delay_days;

    double ym, clean;
    if (in.price_given) {
        clean = in.price;
        ym    = solve_monthly_yield(s, in.settle, clean, delay);
    } else {
        ym    = monthly_equiv(in.yield, in.comp);
        clean = clean_from_yield(s, in.settle, ym, delay);
    }

    Measures m{};
    m.price   = clean;
    m.yield   = from_monthly(ym, in.comp);
    m.accrued = accrued_per_100(s, in.settle);

    /* WAL in years from settle, principal-weighted by receipt date. */
    double num = 0, den = 0;
    for (const Period& p : s.periods) {
        const double prin = p.scheduled_principal + p.prepaid_principal;
        const double t = static_cast<double>(day_diff(in.settle, add_days(p.pay_date, static_cast<int>(delay)))) / 365.25;
        if (t < 0) continue;
        num += t * prin;
        den += prin;
    }
    m.wal = den > 0 ? num / den : 0.0;

    /* Risk by finite difference on the annualised yield, +/- 1bp. */
    const double bump = 0.0001;
    const double y_up = monthly_equiv(m.yield + bump, in.comp);
    const double y_dn = monthly_equiv(m.yield - bump, in.comp);
    const double p0   = pv_per_100(s, in.settle, ym, delay);
    const double p_up = pv_per_100(s, in.settle, y_up, delay);
    const double p_dn = pv_per_100(s, in.settle, y_dn, delay);
    m.dv01         = (p_dn - p_up) / 2.0;
    m.mod_duration = (p_dn - p_up) / (2.0 * p0 * bump);
    m.convexity    = (p_up + p_dn - 2.0 * p0) / (p0 * bump * bump);
    const double freq = in.comp == Compounding::Monthly ? 12.0 : 2.0;
    m.macaulay_duration = m.mod_duration * (1.0 + m.yield / freq);
    return m;
}

} // namespace mbs::core

#endif /* MBS_CORE_ENGINE_HPP */
