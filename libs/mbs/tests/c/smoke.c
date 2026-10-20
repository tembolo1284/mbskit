#include "mbs/mbs.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    mbs_err err = MBS_ERR_INIT;
    mbs_pool_spec sp; memset(&sp, 0, sizeof sp); sp.struct_size = sizeof sp;
    sp.agency = MBS_AGENCY_FNMA; sp.product = MBS_PRODUCT_FIXED_30;
    sp.orig_balance = 100000000.0; sp.wac = 0.060; sp.net_coupon = 0.055;
    sp.orig_term = 360; sp.wam = 357; sp.wala = 3; sp.pay_delay_days = 24;
    sp.next_pay_date = 20261125;
    mbs_pool_t* p = mbs_pool_new(&sp, &err);
    if (!p) { printf("pool_new: %s %s\n", mbs_errc_name(err.code), err.message); return 1; }

    double smm = mbs_cpr_to_smm(0.06);
    mbs_prepay_vector v = { sizeof v, &smm, 1 };
    mbs_prepay_source s = { sizeof s, mbs_prepay_vector_fn, &v };
    mbs_stream_t* st = mbs_pool_project(p, &s, NULL, &err);
    if (!st) { printf("project: %s %s\n", mbs_errc_name(err.code), err.message); return 1; }

    size_t n = 0;
    mbs_stream_copy(st, NULL, 0, 0, &n, &err);
    mbs_cashflow cf[400];
    mbs_stream_copy(st, cf, 400, sizeof cf[0], &n, &err);
    printf("periods=%zu  p1: bal=%.2f sched=%.2f prepay=%.2f int=%.2f end=%.2f\n",
           n, cf[0].begin_balance, cf[0].scheduled_principal, cf[0].prepaid_principal, cf[0].interest, cf[0].end_balance);
    printf("last: period=%u end=%.4f\n", cf[n-1].period, cf[n-1].end_balance);

    mbs_price_inputs in; memset(&in, 0, sizeof in); in.struct_size = sizeof in;
    in.mode = MBS_YIELD_GIVEN; in.settle = 20261020; in.yield = 0.055; in.compounding = MBS_COMP_SEMIANNUAL;
    mbs_measures m;
    if (mbs_stream_measures(st, &in, &m, sizeof m, &err)) { printf("measures: %s\n", err.message); return 1; }
    printf("@5.50 BEY: price=%.4f accrued=%.4f wal=%.2f moddur=%.3f cvx=%.2f dv01=%.4f\n",
           m.price, m.accrued, m.wal, m.mod_duration, m.convexity, m.dv01);

    in.mode = MBS_PRICE_GIVEN; in.price = m.price;
    if (mbs_stream_measures(st, &in, &m, sizeof m, &err)) { printf("measures: %s\n", err.message); return 1; }
    printf("roundtrip yield=%.6f\n", m.yield);

    /* ABI behaviours */
    mbs_cashflow small[10];
    int rc = mbs_stream_copy(st, small, 10, sizeof small[0], &n, &err);
    printf("too-small: rc=%s n=%zu\n", mbs_errc_name(rc), n);
    sp.struct_size = 12; err.code = 0;
    mbs_pool_t* bad = mbs_pool_new(&sp, &err);
    printf("bad size: pool=%p err=%s\n", (void*)bad, mbs_errc_name(err.code));
    mbs_err e2 = MBS_ERR_INIT;
    mbs_cmo_t* c = mbs_cmo_new(NULL,0,NULL,0,&e2);
    printf("unsupported: %p %s / %s\n", (void*)c, mbs_errc_name(e2.code), e2.message);

    mbs_stream_free(st); mbs_pool_free(p); mbs_pool_free(NULL); mbs_stream_free(NULL);
    printf("version %s\n", mbs_version_string());
    return 0;
}
