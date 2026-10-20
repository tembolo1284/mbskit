#include "prepay/prepay.h"

#include "engine.hpp"  // prepay::project
#include "error.hpp"   // prepay::set_last_error / clear_last_error
#include "model.hpp"   // prepay::Model, Scenario, make_model

#include <cstddef>
#include <cstdint>  // SIZE_MAX
#include <exception>
#include <memory>
#include <new>
#include <span>

/*
 * The seam. Every function: clear the thread-local error, validate the C-side
 * preconditions, then run the C++ core inside try/catch. No exception escapes
 * across extern "C"; failures become a prepay_status_t plus a last-error string.
 *
 * Handles are opaque: prepay_model_t* is reinterpret_cast to prepay::Model*
 * (struct prepay_model is never defined). Same trick will apply to scenarios.
 */

extern "C" {

prepay_status_t prepay_model_create(const prepay_model_config_t* cfg,
                                    prepay_model_t**             out_model) {
    prepay::clear_last_error();

    if (cfg == nullptr || out_model == nullptr) {
        prepay::set_last_error("model_create: null argument");
        return PREPAY_ERR_NULL_ARG;
    }
    // Caller's config must be at least as large as the fields we read. A larger
    // struct (newer caller) is fine — we read only our known prefix.
    if (cfg->struct_size < sizeof(prepay_model_config_t)) {
        prepay::set_last_error(
            "model_create: config struct_size smaller than this library "
            "expects (ABI mismatch)");
        return PREPAY_ERR_INVALID_ARG;
    }

    try {
        std::unique_ptr<prepay::Model> m = prepay::make_model(*cfg);
        *out_model = reinterpret_cast<prepay_model_t*>(m.release());
        return PREPAY_OK;
    } catch (const std::bad_alloc&) {
        prepay::set_last_error("model_create: allocation failed");
        return PREPAY_ERR_ALLOC;
    } catch (const std::exception& e) {
        prepay::set_last_error(e.what());  // e.g. "PSA: speed multiple must be >= 0, got -1.5"
        return PREPAY_ERR_INVALID_ARG;
    } catch (...) {
        prepay::set_last_error("model_create: unknown error");
        return PREPAY_ERR_INTERNAL;
    }
}

void prepay_model_destroy(prepay_model_t* model) {
    // delete on null is well-defined; deletes through Model's virtual dtor.
    try {
        delete reinterpret_cast<prepay::Model*>(model);
    } catch (...) {
        // Destructors don't throw, but never let anything cross the boundary.
    }
}

prepay_status_t prepay_project(const prepay_model_t*    model,
                               const prepay_scenario_t* scenario,
                               const prepay_pool_t*     pools,
                               size_t                   n_pools,
                               size_t                   horizon,
                               double*                  out_smm,
                               size_t                   out_len) {
    prepay::clear_last_error();

    if (model == nullptr || pools == nullptr || out_smm == nullptr) {
        prepay::set_last_error("project: null argument");
        return PREPAY_ERR_NULL_ARG;
    }
    if (n_pools == 0 || horizon == 0) {
        prepay::set_last_error("project: n_pools and horizon must be > 0");
        return PREPAY_ERR_INVALID_ARG;
    }
    // Array stride must match our layout, so the pool struct must be our exact
    // size (we check the first row and assume a homogeneous array). A strided
    // entry point is the future-proofing path if mixed sizes ever matter.
    if (pools[0].struct_size != sizeof(prepay_pool_t)) {
        prepay::set_last_error(
            "project: pool struct_size does not match this library's layout "
            "(ABI mismatch)");
        return PREPAY_ERR_INVALID_ARG;
    }
    // Guard the multiply before we trust out_len against it.
    if (horizon > SIZE_MAX / n_pools) {
        prepay::set_last_error("project: n_pools * horizon overflows size_t");
        return PREPAY_ERR_INVALID_ARG;
    }
    if (out_len != n_pools * horizon) {
        prepay::set_last_error("project: out_len must equal n_pools * horizon");
        return PREPAY_ERR_BUFFER_SIZE;
    }

    const auto* m    = reinterpret_cast<const prepay::Model*>(model);
    const auto* scen = reinterpret_cast<const prepay::Scenario*>(scenario);

    if (m->requires_scenario() && scen == nullptr) {
        prepay::set_last_error("project: model requires a scenario but none was given");
        return PREPAY_ERR_UNSUPPORTED;
    }

    try {
        prepay::project(*m, scen,
                        std::span<const prepay_pool_t>(pools, n_pools),
                        horizon,
                        std::span<double>(out_smm, out_len));
        return PREPAY_OK;
    } catch (const std::bad_alloc&) {
        prepay::set_last_error("project: allocation failed");
        return PREPAY_ERR_ALLOC;
    } catch (const std::exception& e) {
        prepay::set_last_error(e.what());
        return PREPAY_ERR_INTERNAL;
    } catch (...) {
        prepay::set_last_error("project: unknown error");
        return PREPAY_ERR_INTERNAL;
    }
}

}  // extern "C"
