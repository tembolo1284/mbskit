#include "model.hpp"

#include <algorithm>
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

// ---- factory ------------------------------------------------------------

std::unique_ptr<Model> make_model(const prepay_model_config_t& cfg) {
    switch (cfg.type) {
        case PREPAY_MODEL_CONST_CPR:
            return std::make_unique<ConstCprModel>(cfg.param, cfg.model_version);
        case PREPAY_MODEL_PSA:
            return std::make_unique<PsaModel>(cfg.param, cfg.model_version);
        default:
            throw std::invalid_argument(
                "unknown prepay_model_type_t: " +
                std::to_string(static_cast<int>(cfg.type)));
    }
}

}  // namespace prepay
