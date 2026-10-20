/* Minimal C driver: exercises the prepay C ABI with no C++/Python in the loop. */
#include <stdio.h>
#include "prepay/prepay.h"

int main(void) {
    printf("version=0x%06x  abi_compatible=%d\n",
           prepay_get_version(), prepay_abi_compatible());

    prepay_model_config_t cfg = {0};
    cfg.struct_size   = sizeof cfg;
    cfg.model_version = 1;
    cfg.type          = PREPAY_MODEL_PSA;
    cfg.param         = 1.0;  /* 100 PSA */

    prepay_model_t *m = NULL;
    if (prepay_model_create(&cfg, &m) != PREPAY_OK) {
        printf("create failed: %s\n", prepay_last_error());
        return 1;
    }

    prepay_pool_t pool = {0};
    pool.struct_size   = sizeof pool;
    pool.original_term = 360;
    pool.age           = 0;
    pool.balance       = 1000000.0;
    pool.wac           = 0.065;
    pool.note_rate     = 0.06;

    double out[6];
    if (prepay_project(m, NULL, &pool, 1, 6, out, 6) != PREPAY_OK) {
        printf("project failed: %s\n", prepay_last_error());
        prepay_model_destroy(m);
        return 1;
    }

    printf("PSA100 SMM:");
    for (int t = 0; t < 6; ++t) printf(" %.8f", out[t]);
    printf("\n");

    prepay_model_destroy(m);
    return 0;
}
