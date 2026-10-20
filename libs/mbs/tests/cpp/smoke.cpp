#include "mbs/mbs.hpp"
#include <cstdio>
int main() {
    auto sp = mbs::make<mbs::PoolSpec>();
    sp.agency = MBS_AGENCY_FNMA; sp.product = MBS_PRODUCT_FIXED_30;
    sp.orig_balance = 1e8; sp.wac = 0.060; sp.net_coupon = 0.055;
    sp.orig_term = 360; sp.wam = 357; sp.wala = 3; sp.pay_delay_days = 24; sp.next_pay_date = 20261125;
    mbs::Pool pool(sp);

    auto st = pool.project(mbs::PrepaySource::psa(1.0));
    auto cfs = st.cashflows();
    auto m = st.priceFromYield(20261020, 0.055);
    std::printf("100PSA: n=%zu price=%.4f wal=%.2f dur=%.3f\n", cfs.size(), m.price, m.wal, m.mod_duration);

    /* exception from user callback crosses the waist and comes back as itself */
    try {
        pool.project(mbs::PrepaySource([](const mbs::PrepayCtx& c) -> double {
            if (c.period == 7) throw std::logic_error("boom at 7");
            return 0.01;
        }));
    } catch (const std::logic_error& e) { std::printf("user exception recovered: %s\n", e.what()); }

    /* library error becomes mbs::Error */
    try { sp.wac = 0; mbs::Pool bad(sp); }
    catch (const mbs::Error& e) { std::printf("mbs::Error code=%d what=%s\n", e.code(), e.what()); }

    try { mbs::Cmo cmo({&pool}, {}); }
    catch (const mbs::Error& e) { std::printf("cmo: %s\n", e.what()); }
    return 0;
}
