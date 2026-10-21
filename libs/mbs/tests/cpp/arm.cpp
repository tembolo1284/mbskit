#include "mbs/mbs.hpp"
#include <cmath>
#include <cstdio>
#include <vector>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("FAIL: %s\n", m); ++fails; } } while (0)

int main() {
    auto base = mbs::make<mbs::PoolSpec>();
    base.agency = MBS_AGENCY_FNMA; base.product = MBS_PRODUCT_ARM;
    base.orig_balance = 1e6; base.wac = 0.0450; base.net_coupon = 0.0400;
    base.orig_term = 360; base.wam = 360; base.pay_delay_days = 24; base.next_pay_date = 20261125;

    auto a = mbs::make<mbs::ArmSpec>();
    a.index = MBS_INDEX_CMT_1Y; a.margin = 0.0225;
    a.months_to_first_reset = 60; a.reset_freq_months = 12; a.lookback_days = 45;
    a.initial_cap = 0.02; a.periodic_cap = 0.02; a.lifetime_cap = 0.0950; a.rounding = 0.00125;

    mbs::Arm arm(base, a);

    std::vector<double> idx(400, 0.03);
    for (size_t i = 57; i < idx.size(); ++i) idx[i] = 0.08;   /* 2-period lookback */

    /* prepay model that sees the ARM's reset coupon through the ctx */
    double max_seen = 0;
    auto st = arm.project(mbs::PrepaySource([&](const mbs::PrepayCtx& c) {
        max_seen = std::max(max_seen, c.current_coupon);
        return mbs::cprToSmm(0.06);
    }), mbs::RatePath(idx));

    auto cfs = st.cashflows();
    CHECK(cfs.size() == 360, "360 periods");
    CHECK(std::fabs(cfs[59].coupon - 0.0600) < 1e-12, "p60 net coupon 6.00");
    CHECK(std::fabs(max_seen - 0.0950) < 1e-12, "callback saw lifetime-capped gross 9.50");

    auto m = st.priceFromYield(20261020, 0.055);
    CHECK(m.price > 90 && m.price < 115, "price sane");
    std::printf("5/1 hybrid @5.50 BEY: price=%.4f wal=%.2f dur=%.3f\n", m.price, m.wal, m.mod_duration);

    /* missing path -> mbs::Error INVALID_ARG */
    try { arm.project(mbs::PrepaySource::constantCpr(0.06), mbs::RatePath()); CHECK(false, "should throw"); }
    catch (const mbs::Error& e) { CHECK(e.code() == MBS_ERRC_INVALID_ARG, "empty path error code"); }

    /* floor > cap rejected at construction */
    a.lifetime_floor = 0.10;
    try { mbs::Arm bad(base, a); CHECK(false, "should throw"); }
    catch (const mbs::Error& e) { CHECK(e.code() == MBS_ERRC_INVALID_ARG, "floor>cap rejected"); }

    std::printf(fails ? "arm.cpp: %d failure(s)\n" : "arm.cpp: ok\n", fails);
    return fails ? 1 : 0;
}
