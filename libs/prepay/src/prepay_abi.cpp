#include "prepay/prepay.h"

#include "engine.hpp"    // prepay::project
#include "error.hpp"     // prepay::set_last_error / clear_last_error
#include "model.hpp"     // prepay::Model, make_model
#include "scenario.hpp"  // prepay::Scenario

#include <algorithm>  // std::min
#include <cstddef>    // offsetof
#include <cstdint>    // SIZE_MAX
#include <cstring>    // std::memcpy
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <string>

/*
 * The seam. Every function: clear the thread-local error, validate the C-side
 * preconditions, then run the C++ core inside try/catch. No exception escapes
 * across extern "C"; failures become a prepay_status_t plus a last-error string.
 *
 * Handles are opaque: prepay_model_t* is reinterpret_cast to prepay::Model*
 * and prepay_scenario_t* to prepay::Scenario* (neither struct is ever defined).
 */

namespace {

// The 0.1.0 config layout ends where the 0.2.0 S-curve fields begin (24 bytes).
// Any caller stamping at least this much is understood; we zero-fill the rest.
constexpr std::size_t kModelConfigMinSize =
    offsetof(prepay_model_config_t, scurve_floor);

}  // namespace

extern "C" {

prepay_status_t prepay_model_create(const prepay_model_config_t* cfg,
                                    prepay_model_t**             out_model) {
    prepay::clear_last_error();

    if (cfg == nullptr || out_model == nullptr) {
        prepay::set_last_error("model_create: null argument");
        return PREPAY_ERR_NULL_ARG;
    }
    // Only the 0.1.0 prefix is mandatory. Anything shorter predates the first
    // shipped layout and can't be interpreted.
    if (cfg->struct_size < kModelConfigMinSize) {
        prepay::set_last_error(
            "model_create: config struct_size smaller than this library's "
            "minimum supported layout (ABI mismatch)");
        return PREPAY_ERR_INVALID_ARG;
    }

    // Normalize the caller's config onto our layout. An older caller's short
    // struct gets zero-filled appended fields; a newer caller's longer struct
    // is truncated to the prefix we understand. This memcpy is the append-only
    // promise actually being kept — without it, growing the struct would break
    // every already-compiled caller.
    prepay_model_config_t local{};
    const std::size_t     copy_n =
        std::min<std::size_t>(cfg->struct_size, sizeof(local));
    std::memcpy(&local, cfg, copy_n);
    local.struct_size = static_cast<uint32_t>(copy_n);

    // Models introduced in 0.2.0 read fields a 0.1.0 caller never stamped.
    // Zero-filled S-curve params are a *valid-looking* config that would
    // silently project a flat 0% CPR, so reject instead of guessing.
    if (local.type == PREPAY_MODEL_REFI_SCURVE &&
        copy_n < sizeof(prepay_model_config_t)) {
        prepay::set_last_error(
            "model_create: REFI_SCURVE requires a 0.2.0 config (struct_size "
            "too small to carry the S-curve parameters)");
        return PREPAY_ERR_INVALID_ARG;
    }

    try {
        std::unique_ptr<prepay::Model> m = prepay::make_model(local);
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

prepay_status_t prepay_scenario_create(const prepay_scenario_config_t* cfg,
                                       prepay_scenario_t**             out_scenario) {
    prepay::clear_last_error();

    if (cfg == nullptr || out_scenario == nullptr) {
        prepay::set_last_error("scenario_create: null argument");
        return PREPAY_ERR_NULL_ARG;
    }
    // No appended fields yet, so the whole struct is the minimum. When this one
    // grows, it gets the same prefix-copy treatment as the model config.
    if (cfg->struct_size < sizeof(prepay_scenario_config_t)) {
        prepay::set_last_error(
            "scenario_create: config struct_size smaller than this library "
            "expects (ABI mismatch)");
        return PREPAY_ERR_INVALID_ARG;
    }
    if (cfg->mortgage_rate == nullptr) {
        prepay::set_last_error("scenario_create: mortgage_rate is null");
        return PREPAY_ERR_NULL_ARG;
    }
    if (cfg->n_months == 0) {
        prepay::set_last_error("scenario_create: n_months must be > 0");
        return PREPAY_ERR_INVALID_ARG;
    }

    try {
        // Scenario copies the path; cfg->mortgage_rate is dead to us on return.
        auto s = std::make_unique<prepay::Scenario>(
            std::span<const double>(cfg->mortgage_rate, cfg->n_months),
            cfg->scenario_id);
        *out_scenario = reinterpret_cast<prepay_scenario_t*>(s.release());
        return PREPAY_OK;
    } catch (const std::bad_alloc&) {
        prepay::set_last_error("scenario_create: allocation failed");
        return PREPAY_ERR_ALLOC;
    } catch (const std::exception& e) {
        prepay::set_last_error(e.what());  // e.g. "scenario: mortgage_rate[7] must be ..."
        return PREPAY_ERR_INVALID_ARG;
    } catch (...) {
        prepay::set_last_error("scenario_create: unknown error");
        return PREPAY_ERR_INTERNAL;
    }
}

void prepay_scenario_destroy(prepay_scenario_t* scenario) {
    try {
        delete reinterpret_cast<prepay::Scenario*>(scenario);
    } catch (...) {
        // Never let anything cross the boundary.
    }
}

prepay_status_t prepay_model_version(const prepay_model_t* model,
                                     uint32_t*             out_version) {
    prepay::clear_last_error();

    if (model == nullptr || out_version == nullptr) {
        prepay::set_last_error("model_version: null argument");
        return PREPAY_ERR_NULL_ARG;
    }
    *out_version = reinterpret_cast<const prepay::Model*>(model)->model_version();
    return PREPAY_OK;
}

prepay_status_t prepay_scenario_id(const prepay_scenario_t* scenario,
                                   uint32_t*                out_id) {
    prepay::clear_last_error();

    if (scenario == nullptr || out_id == nullptr) {
        prepay::set_last_error("scenario_id: null argument");
        return PREPAY_ERR_NULL_ARG;
    }
    *out_id = reinterpret_cast<const prepay::Scenario*>(scenario)->scenario_id();
    return PREPAY_OK;
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

    // Establish the two preconditions RefiScurveModel asserts on: a scenario
    // exists, and its path covers every month we're about to index.
    if (m->requires_scenario()) {
        if (scen == nullptr) {
            prepay::set_last_error(
                "project: model requires a scenario but none was given");
            return PREPAY_ERR_UNSUPPORTED;
        }
        if (scen->n_months() < horizon) {
            prepay::set_last_error(
                "project: scenario rate path has " +
                std::to_string(scen->n_months()) +
                " months, shorter than horizon " + std::to_string(horizon));
            return PREPAY_ERR_SHORT_PATH;
        }
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
