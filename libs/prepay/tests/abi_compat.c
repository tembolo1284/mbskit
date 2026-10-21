/*
 * abi_compat.c - proof that the append-only promise actually holds.
 *
 * The 0.2.0 release grew prepay_model_config_t from 24 to 64 bytes. A caller
 * compiled against the 0.1.0 header still stamps struct_size = 24 and passes a
 * 24-byte object. This test freezes that old layout, hands it to the current
 * binary, and checks three things:
 *
 *   1. Field offsets never moved          (compile time, _Static_assert)
 *   2. The library reads nothing past struct_size (runtime, poisoned memory)
 *   3. An old caller asking for a new model is rejected, not silently wrong
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -Werror -Iinclude tests/abi_compat.c \
 *      -Lbuild-native -lprepay -Wl,-rpath,"$PWD/build-native" \
 *      -o build-native/abi_compat
 */

#include "prepay/prepay.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---- the frozen 0.1.0 layout -------------------------------------------
 * Hand-copied from the 0.1.0 header and never edited again. If a future
 * release moves a field, the static asserts below stop the build. */
typedef struct {
    uint32_t            struct_size;
    uint32_t            model_version;
    prepay_model_type_t type;
    double              param;
} prepay_model_config_v1_t;

/* The 0.1.0 struct was 24 bytes: 4 + 4 + 4 + (4 pad) + 8. */
_Static_assert(sizeof(prepay_model_config_v1_t) == 24,
               "0.1.0 config layout changed - this file must stay frozen");

/* Every 0.1.0 field sits at the same offset in the 0.2.0 struct. This is the
 * whole append-only contract, checked at compile time. */
_Static_assert(offsetof(prepay_model_config_t, struct_size) ==
               offsetof(prepay_model_config_v1_t, struct_size),
               "struct_size moved");
_Static_assert(offsetof(prepay_model_config_t, model_version) ==
               offsetof(prepay_model_config_v1_t, model_version),
               "model_version moved");
_Static_assert(offsetof(prepay_model_config_t, type) ==
               offsetof(prepay_model_config_v1_t, type),
               "type moved");
_Static_assert(offsetof(prepay_model_config_t, param) ==
               offsetof(prepay_model_config_v1_t, param),
               "param moved");

/* New fields begin exactly where the old struct ended - nothing was wedged
 * into the middle. */
_Static_assert(offsetof(prepay_model_config_t, scurve_floor) ==
               sizeof(prepay_model_config_v1_t),
               "0.2.0 fields must be appended, not inserted");

/* The pool struct has NOT grown; if it ever does, it needs this same drill. */
_Static_assert(offsetof(prepay_pool_t, struct_size)   ==  0, "pool layout changed");
_Static_assert(offsetof(prepay_pool_t, original_term) ==  4, "pool layout changed");
_Static_assert(offsetof(prepay_pool_t, age)           ==  8, "pool layout changed");
_Static_assert(offsetof(prepay_pool_t, balance)       == 16, "pool layout changed");
_Static_assert(offsetof(prepay_pool_t, wac)           == 24, "pool layout changed");
_Static_assert(offsetof(prepay_pool_t, note_rate)     == 32, "pool layout changed");

/* ---- tiny harness ------------------------------------------------------ */
static int g_failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s  [%s]\n", what, prepay_last_error());
        g_failures++;
    }
}

#define HORIZON 12u

static prepay_pool_t make_pool(void) {
    prepay_pool_t p;
    memset(&p, 0, sizeof p);
    p.struct_size   = (uint32_t)sizeof(prepay_pool_t);
    p.original_term = 360;
    p.age           = 0;
    p.balance       = 1000000.0;
    p.wac           = 0.065;
    p.note_rate     = 0.065;
    return p;
}

/* ---- 1. an 0.1.0 caller still works ------------------------------------ */
static void test_old_caller_psa(void) {
    printf("0.1.0 caller, PSA:\n");

    /* A 64-byte region poisoned with 0xA5, with only the first 24 bytes set to
     * a valid 0.1.0 config. If the library reads past struct_size it sees
     * garbage - and 0xA5 repeated is a signalling-ish double, not a quiet
     * zero, so the failure would be loud rather than plausible. */
    _Alignas(prepay_model_config_t)
        unsigned char raw[sizeof(prepay_model_config_t)];
    memset(raw, 0xA5, sizeof raw);

    prepay_model_config_v1_t v1;
    memset(&v1, 0, sizeof v1);
    v1.struct_size   = (uint32_t)sizeof v1;   /* 24, as 0.1.0 stamped it */
    v1.model_version = 101u;
    v1.type          = PREPAY_MODEL_PSA;
    v1.param         = 1.0;                   /* 100 PSA */
    memcpy(raw, &v1, sizeof v1);

    prepay_model_t *old_model = NULL;
    prepay_status_t st = prepay_model_create(
        (const prepay_model_config_t *)(const void *)raw, &old_model);
    check(st == PREPAY_OK, "0.1.0 config (struct_size=24) is accepted");
    if (st != PREPAY_OK) return;

    uint32_t stamped = 0;
    check(prepay_model_version(old_model, &stamped) == PREPAY_OK &&
          stamped == 101u,
          "model_version survives the short-struct path");

    const prepay_pool_t pool = make_pool();
    double old_smm[HORIZON];
    st = prepay_project(old_model, NULL, &pool, 1, HORIZON,
                        old_smm, HORIZON);
    check(st == PREPAY_OK, "projection from a 0.1.0 model handle");

    /* Same model built the 0.2.0 way must produce bit-identical output: the
     * zero-filled appended fields are genuinely ignored for PSA. */
    prepay_model_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size   = (uint32_t)sizeof cfg;
    cfg.model_version = 101u;
    cfg.type          = PREPAY_MODEL_PSA;
    cfg.param         = 1.0;

    prepay_model_t *new_model = NULL;
    double          new_smm[HORIZON];
    if (prepay_model_create(&cfg, &new_model) == PREPAY_OK) {
        prepay_project(new_model, NULL, &pool, 1, HORIZON, new_smm, HORIZON);
        check(memcmp(old_smm, new_smm, sizeof old_smm) == 0,
              "0.1.0 and 0.2.0 configs give identical PSA output");
        prepay_model_destroy(new_model);
    } else {
        check(0, "0.2.0 PSA config accepted");
    }

    prepay_model_destroy(old_model);
}

/* ---- 2. an 0.1.0 caller cannot reach a 0.2.0 model ---------------------- */
static void test_old_caller_cannot_scurve(void) {
    printf("0.1.0 caller, REFI_SCURVE:\n");

    /* A short struct carries no S-curve parameters. Zero-filling them would
     * build a model that projects a flat 0% CPR and looks fine - so the
     * library must refuse instead of guessing. */
    prepay_model_config_v1_t v1;
    memset(&v1, 0, sizeof v1);
    v1.struct_size = (uint32_t)sizeof v1;
    v1.type        = PREPAY_MODEL_REFI_SCURVE;

    prepay_model_t *m  = NULL;
    prepay_status_t st = prepay_model_create(
        (const prepay_model_config_t *)(const void *)&v1, &m);
    check(st == PREPAY_ERR_INVALID_ARG,
          "REFI_SCURVE from a 24-byte config is rejected");
    check(m == NULL, "out_model left untouched on failure");

    /* And anything shorter than the 0.1.0 prefix is not interpretable at all. */
    prepay_model_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = 8;
    cfg.type        = PREPAY_MODEL_PSA;
    cfg.param       = 1.0;
    prepay_model_t *tiny = NULL;
    check(prepay_model_create(&cfg, &tiny) == PREPAY_ERR_INVALID_ARG,
          "struct_size below the 0.1.0 prefix is rejected");
}

/* ---- 3. the 0.2.0 path end to end -------------------------------------- */
static void test_scurve_and_scenario(void) {
    printf("0.2.0 scenario + REFI_SCURVE:\n");

    double path[24];
    for (size_t i = 0; i < 24; ++i) path[i] = 0.050;  /* 125bp in the money */

    prepay_scenario_config_t scfg;
    memset(&scfg, 0, sizeof scfg);
    scfg.struct_size   = (uint32_t)sizeof scfg;
    scfg.scenario_id   = 7u;
    scfg.mortgage_rate = path;
    scfg.n_months      = 24;

    prepay_scenario_t *scen = NULL;
    check(prepay_scenario_create(&scfg, &scen) == PREPAY_OK,
          "scenario_create");
    if (scen == NULL) return;

    /* The library copied the path; corrupting ours must not matter. */
    for (size_t i = 0; i < 24; ++i) path[i] = -999.0;

    uint32_t sid = 0;
    check(prepay_scenario_id(scen, &sid) == PREPAY_OK && sid == 7u,
          "scenario_id round-trips");

    prepay_model_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size            = (uint32_t)sizeof cfg;
    cfg.model_version          = 200u;
    cfg.type                   = PREPAY_MODEL_REFI_SCURVE;
    cfg.scurve_floor           = 0.05;
    cfg.scurve_ceiling         = 0.45;
    cfg.scurve_midpoint        = 0.0075;
    cfg.scurve_steepness       = 400.0;
    cfg.scurve_seasoning_months = 0.0;   /* ramp disabled */

    prepay_model_t *m = NULL;
    check(prepay_model_create(&cfg, &m) == PREPAY_OK, "REFI_SCURVE model_create");
    if (m == NULL) { prepay_scenario_destroy(scen); return; }

    const prepay_pool_t pool = make_pool();
    double smm[HORIZON];

    check(prepay_project(m, scen, &pool, 1, HORIZON, smm, HORIZON) == PREPAY_OK,
          "projection with a scenario");
    /* 125bp of incentive against a 75bp midpoint sits above half height, so
     * CPR must be above the midpoint of floor..ceiling (0.25 annual). */
    check(smm[0] > 0.0 && smm[0] < 1.0, "SMM is a sane probability");
    check(1.0 - pow(1.0 - smm[0], 12.0) > 0.25,
          "in-the-money pool prepays above half height");

    prepay_model_t *psa = NULL;
    cfg.type  = PREPAY_MODEL_PSA;
    cfg.param = 1.0;
    if (prepay_model_create(&cfg, &psa) == PREPAY_OK) {
        check(prepay_project(psa, NULL, &pool, 1, HORIZON, smm, HORIZON) == PREPAY_OK,
              "rate-independent model still accepts a NULL scenario");
        prepay_model_destroy(psa);
    }

    check(prepay_project(m, NULL, &pool, 1, HORIZON, smm, HORIZON) ==
          PREPAY_ERR_UNSUPPORTED,
          "REFI_SCURVE with no scenario -> UNSUPPORTED");

    double long_smm[48];
    check(prepay_project(m, scen, &pool, 1, 48, long_smm, 48) ==
          PREPAY_ERR_SHORT_PATH,
          "horizon past the rate path -> SHORT_PATH");

    prepay_model_destroy(m);
    prepay_scenario_destroy(scen);
    prepay_scenario_destroy(NULL);  /* must be a no-op */
}

int main(void) {
    printf("abi_compat: header %u.%u.%u, binary 0x%06x, compatible=%d\n\n",
           PREPAY_VERSION_MAJOR, PREPAY_VERSION_MINOR, PREPAY_VERSION_PATCH,
           prepay_get_version(), prepay_abi_compatible());

    test_old_caller_psa();
    test_old_caller_cannot_scurve();
    test_scurve_and_scenario();

    printf("\n%s\n", g_failures == 0 ? "abi_compat passed"
                                     : "abi_compat FAILED");
    return g_failures == 0 ? 0 : 1;
}
