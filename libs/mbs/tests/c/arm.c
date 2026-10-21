/* ARM reset mechanics through the C waist. 5/1 hybrid: 5yr teaser at 4.50%,
 * then annual resets off a 1Y CMT path with 2/2/5 caps. */
#include "mbs/mbs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)
#define NEAR(a, b, tol, msg) CHECK(fabs((a) - (b)) <= (tol), msg)

static int zero_smm(const mbs_prepay_ctx* c, void* u, double* out) { (void)c; (void)u; *out = 0.0; return 0; }

int main(void) {
    mbs_err err = MBS_ERR_INIT;

    mbs_pool_spec base; memset(&base, 0, sizeof base); base.struct_size = sizeof base;
    base.agency = MBS_AGENCY_FNMA; base.product = MBS_PRODUCT_ARM;
    base.orig_balance = 1e6; base.wac = 0.0450; base.net_coupon = 0.0400;
    base.orig_term = 360; base.wam = 360; base.wala = 0; base.pay_delay_days = 24;
    base.next_pay_date = 20261125;

    mbs_arm_spec arm; memset(&arm, 0, sizeof arm); arm.struct_size = sizeof arm;
    arm.index = MBS_INDEX_CMT_1Y; arm.margin = 0.0225;
    arm.months_to_first_reset = 60; arm.reset_freq_months = 12; arm.lookback_days = 45;
    arm.initial_cap = 0.02; arm.periodic_cap = 0.02; arm.lifetime_cap = 0.0950; arm.lifetime_floor = 0.0225;
    arm.rounding = 0.00125;

    /* wrong product must be rejected */
    base.product = MBS_PRODUCT_FIXED_30;
    CHECK(mbs_arm_new(&base, &arm, &err) == NULL && err.code == MBS_ERRC_INVALID_ARG, "fixed product rejected");
    base.product = MBS_PRODUCT_ARM; mbs_err_init(&err);

    mbs_arm_t* a = mbs_arm_new(&base, &arm, &err);
    CHECK(a != NULL, err.message);
    if (!a) return 1;

    /* no path -> INVALID_ARG */
    mbs_prepay_source src = { sizeof src, zero_smm, NULL };
    CHECK(mbs_arm_project(a, &src, NULL, &err) == NULL && err.code == MBS_ERRC_INVALID_ARG, "null path rejected");
    mbs_err_init(&err);

    /* lookback_days 45 -> the p60 reset observes the index at p58. Step the
     * index from 3% to 8% at p58 so the reset sees 8%: fully indexed 10.25%,
     * but initial cap holds it to 6.50%, next reset to 8.50%, then the 9.50%
     * lifetime cap binds. */
    double idx[400];
    for (int i = 0; i < 400; ++i) idx[i] = i < 57 ? 0.03 : 0.08;
    mbs_rate_path path = { sizeof path, idx, 400 };

    mbs_stream_t* st = mbs_arm_project(a, &src, &path, &err);
    CHECK(st != NULL, err.message);
    if (!st) return 1;

    size_t n = 0; mbs_cashflow cf[400];
    mbs_stream_copy(st, cf, 400, sizeof cf[0], &n, &err);
    CHECK(n == 360, "360 periods with zero prepay");

    /* net coupon = gross - 0.50% spread */
    NEAR(cf[0].coupon,   0.0400, 1e-12, "teaser net coupon p1");
    NEAR(cf[58].coupon,  0.0400, 1e-12, "teaser holds through p59");
    NEAR(cf[59].coupon,  0.0600, 1e-12, "p60 reset: 4.50 + 2.00 initial cap = 6.50 gross -> 6.00 net");
    NEAR(cf[70].coupon,  0.0600, 1e-12, "held between resets");
    NEAR(cf[71].coupon,  0.0800, 1e-12, "p72 reset: +2.00 periodic cap -> 8.50 gross -> 8.00 net");
    NEAR(cf[83].coupon,  0.0900, 1e-12, "p84 reset: lifetime cap 9.50 gross -> 9.00 net");
    NEAR(cf[95].coupon,  0.0900, 1e-12, "p96 stays at lifetime cap");

    /* payment re-levels at reset: scheduled principal drops when coupon rises */
    CHECK(cf[59].scheduled_principal < cf[58].scheduled_principal, "sched principal falls when coupon rises");
    /* balance still amortises to zero */
    NEAR(cf[359].end_balance, 0.0, 1e-6, "amortises to zero");
    /* interest reflects the new coupon */
    NEAR(cf[59].interest, cf[59].begin_balance * 0.0600 / 12.0, 1e-6, "p60 interest at new net coupon");

    /* lookback: step the index at p59 instead (one period too late for the
     * p60 reset) and the reset must still see 3% -> 5.25 gross / 4.75 net. */
    for (int i = 0; i < 400; ++i) idx[i] = i < 58 ? 0.03 : 0.08;
    mbs_stream_free(st);
    st = mbs_arm_project(a, &src, &path, &err);
    CHECK(st != NULL, err.message);
    mbs_stream_copy(st, cf, 400, sizeof cf[0], &n, &err);
    NEAR(cf[59].coupon, 0.0475, 1e-12, "45-day lookback: p60 reset observes p58 index");

    /* falling index: 1% throughout -> 3.25% fully indexed. The 2.00 cap does
     * not bind on a 1.25 move, floor 2.25% < 3.25% does not bind either, so
     * p60 goes straight to 3.25 gross / 2.75 net and holds. */
    for (int i = 0; i < 400; ++i) idx[i] = 0.01;
    mbs_stream_free(st);
    st = mbs_arm_project(a, &src, &path, &err);
    CHECK(st != NULL, err.message);
    mbs_stream_copy(st, cf, 400, sizeof cf[0], &n, &err);
    NEAR(cf[59].coupon, 0.0275, 1e-12, "p60 down-reset to fully indexed 3.25 gross -> 2.75 net");
    NEAR(cf[71].coupon, 0.0275, 1e-12, "p72 holds at fully indexed");

    /* now make the floor bind: lifetime_floor 3.75% > 3.25% fully indexed */
    arm.lifetime_floor = 0.0375;
    mbs_arm_free(a);
    a = mbs_arm_new(&base, &arm, &err);
    CHECK(a != NULL, err.message);
    mbs_stream_free(st);
    st = mbs_arm_project(a, &src, &path, &err);
    CHECK(st != NULL, err.message);
    mbs_stream_copy(st, cf, 400, sizeof cf[0], &n, &err);
    NEAR(cf[59].coupon, 0.0325, 1e-12, "lifetime floor binds: 3.75 gross -> 3.25 net");

    /* and a big down move where the periodic cap binds: index 0% -> fully
     * indexed 2.25 but |2.25 - 4.50| = 2.25 > 2.00 cap => 2.50 gross */
    arm.lifetime_floor = 0.0;
    for (int i = 0; i < 400; ++i) idx[i] = 0.0;
    mbs_arm_free(a);
    a = mbs_arm_new(&base, &arm, &err);
    mbs_stream_free(st);
    st = mbs_arm_project(a, &src, &path, &err);
    CHECK(st != NULL, err.message);
    mbs_stream_copy(st, cf, 400, sizeof cf[0], &n, &err);
    NEAR(cf[59].coupon, 0.0200, 1e-12, "p60 down move capped at -2.00: 2.50 gross -> 2.00 net");
    NEAR(cf[71].coupon, 0.0175, 1e-12, "p72 reaches fully indexed 2.25 gross -> 1.75 net");

    /* get_spec round-trips */
    mbs_pool_spec b2; memset(&b2, 0, sizeof b2); b2.struct_size = sizeof b2;
    mbs_arm_spec a2; memset(&a2, 0, sizeof a2); a2.struct_size = sizeof a2;
    CHECK(mbs_arm_get_spec(a, &b2, &a2, &err) == 0, "get_spec");
    NEAR(a2.margin, 0.0225, 1e-15, "spec margin round-trip");
    NEAR(b2.factor, 1.0, 1e-15, "normalised factor");

    mbs_stream_free(st); mbs_arm_free(a); mbs_arm_free(NULL);
    printf(fails ? "arm: %d failure(s)\n" : "arm: ok\n", fails);
    return fails ? 1 : 0;
}
