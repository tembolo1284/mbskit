#include "model.hpp"

#include "scenario.hpp"  // prepay::Scenario (only the .cpp needs the definition)

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace prepay {
namespace {

// Annualized CPR -> single monthly mortality: SMM = 1 - (1 - CPR)^(1/12).
// Assumes cpr already clamped to [0, 1].
inline double cpr_to_smm(double cpr) noexcept {
    return 1.0 - std::pow(1.0 - cpr, 1.0 / 12.0);
}

}  // namespace

// ---- ConstCprModel ------------------------------------------------------

ConstCprModel::ConstCprModel(double annual_cpr, std::uint32_t model_version)
    : Model(model_version) {
    if (!(annual_cpr >= 0.0 && annual_cpr <= 1.0)) {  // also rejects NaN
        throw std::invalid_argument(
            "CONST_CPR: annual CPR must be in [0, 1], got " +
            std::to_string(annual_cpr));
    }
    smm_ = cpr_to_smm(annual_cpr);
}

void ConstCprModel::project_pool(const prepay_pool_t& /*pool*/,
                                 const Scenario* /*scenario*/,
                                 std::span<double> out_smm) const {
    std::fill(out_smm.begin(), out_smm.end(), smm_);
}

// ---- PsaModel -----------------------------------------------------------

PsaModel::PsaModel(double speed, std::uint32_t model_version)
    : Model(model_version), speed_(speed) {
    // Reject negatives and NaN; no upper bound (e.g. 600 PSA == speed 6.0 is valid).
    if (!(speed >= 0.0)) {
        throw std::invalid_argument(
            "PSA: speed multiple must be >= 0, got " + std::to_string(speed));
    }
}

void PsaModel::project_pool(const prepay_pool_t& pool,
                            const Scenario* /*scenario*/,
                            std::span<double> out_smm) const {
    // Seasoning convention: output index t is the loan's (pool.age + t + 1)-th
    // month of life, so a new pool (age 0) starts at PSA month 1. At 100 PSA,
    // CPR ramps 0.2%/month to 6% at month 30, then stays flat.
    constexpr double        kRampStepCpr = 0.002;  // 0.2% annual CPR per month
    constexpr std::uint64_t kRampMonths  = 30;

    const std::size_t horizon = out_smm.size();
    for (std::size_t t = 0; t < horizon; ++t) {
        const std::uint64_t psa_age =
            static_cast<std::uint64_t>(pool.age) + t + 1;
        const std::uint64_t ramp = std::min(psa_age, kRampMonths);
        double cpr = speed_ * kRampStepCpr * static_cast<double>(ramp);
        if (cpr > 1.0) cpr = 1.0;  // guard extreme speeds
        out_smm[t] = cpr_to_smm(cpr);
    }
}

// ---- RefiScurveModel ----------------------------------------------------

RefiScurveModel::RefiScurveModel(const Params& params, std::uint32_t model_version)
    : Model(model_version), params_(params) {
    const auto bad = [](double v) { return !std::isfinite(v); };

    if (bad(params.floor) || params.floor < 0.0 || params.floor > 1.0) {
        throw std::invalid_argument(
            "REFI_SCURVE: floor must be in [0, 1], got " +
            std::to_string(params.floor));
    }
    if (bad(params.ceiling) || params.ceiling < 0.0 || params.ceiling > 1.0) {
        throw std::invalid_argument(
            "REFI_SCURVE: ceiling must be in [0, 1], got " +
            std::to_string(params.ceiling));
    }
    if (params.ceiling < params.floor) {
        throw std::invalid_argument(
            "REFI_SCURVE: ceiling (" + std::to_string(params.ceiling) +
            ") must be >= floor (" + std::to_string(params.floor) + ")");
    }
    if (bad(params.midpoint)) {
        throw std::invalid_argument(
            "REFI_SCURVE: midpoint must be finite, got " +
            std::to_string(params.midpoint));
    }
    if (bad(params.steepness) || params.steepness < 0.0) {
        throw std::invalid_argument(
            "REFI_SCURVE: steepness must be finite and >= 0, got " +
            std::to_string(params.steepness));
    }
    if (bad(params.seasoning_months) || params.seasoning_months < 0.0) {
        throw std::invalid_argument(
            "REFI_SCURVE: seasoning_months must be finite and >= 0, got " +
            std::to_string(params.seasoning_months));
    }
}

void RefiScurveModel::project_pool(const prepay_pool_t& pool,
                                   const Scenario*      scenario,
                                   std::span<double>    out_smm) const {
    // Both preconditions are enforced at the ABI boundary (UNSUPPORTED and
    // SHORT_PATH respectively), so this is a debug-only tripwire for internal
    // callers, not a runtime check on the hot path.
    assert(scenario != nullptr &&
           "RefiScurveModel: scenario must be non-null (shim precondition)");
    assert(scenario->n_months() >= out_smm.size() &&
           "RefiScurveModel: rate path shorter than horizon (shim precondition)");

    const double range   = params_.ceiling - params_.floor;
    const bool   seasons = params_.seasoning_months > 1.0;

    const std::size_t horizon = out_smm.size();
    for (std::size_t t = 0; t < horizon; ++t) {
        const double incentive = pool.wac - scenario->mortgage_rate(t);

        // Clamp the exponent rather than letting exp() overflow to inf. The
        // logistic is saturated to within ~1e-17 long before |x| = 40, so this
        // costs nothing in accuracy and removes a whole class of inf/NaN.
        double x = params_.steepness * (incentive - params_.midpoint);
        x = std::clamp(x, -40.0, 40.0);
        const double logistic = 1.0 / (1.0 + std::exp(-x));

        double cpr = params_.floor + range * logistic;

        if (seasons) {
            // Same convention as PSA: output index t is the loan's
            // (pool.age + t + 1)-th month of life.
            const double life = static_cast<double>(pool.age) +
                                static_cast<double>(t) + 1.0;
            cpr *= std::min(1.0, life / params_.seasoning_months);
        }

        out_smm[t] = cpr_to_smm(std::clamp(cpr, 0.0, 1.0));
    }
}

// ---- factory ------------------------------------------------------------

std::unique_ptr<Model> make_model(const prepay_model_config_t& cfg) {
    switch (cfg.type) {
        case PREPAY_MODEL_CONST_CPR:
            return std::make_unique<ConstCprModel>(cfg.param, cfg.model_version);
        case PREPAY_MODEL_PSA:
            return std::make_unique<PsaModel>(cfg.param, cfg.model_version);
        case PREPAY_MODEL_REFI_SCURVE: {
            const RefiScurveModel::Params p{cfg.scurve_floor,
                                            cfg.scurve_ceiling,
                                            cfg.scurve_midpoint,
                                            cfg.scurve_steepness,
                                            cfg.scurve_seasoning_months};
            return std::make_unique<RefiScurveModel>(p, cfg.model_version);
        }
        default:
            throw std::invalid_argument(
                "unknown prepay_model_type_t: " +
                std::to_string(static_cast<int>(cfg.type)));
    }
}

}  // namespace prepay
