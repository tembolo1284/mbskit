#ifndef PREPAY_SRC_MODEL_HPP_INCLUDED
#define PREPAY_SRC_MODEL_HPP_INCLUDED

/*
 * Private C++ model interface (not shipped; not in the public include dir).
 * Free to use modern C++ here — nothing in this header crosses the extern "C"
 * boundary. The ABI shim reinterpret_casts the opaque handle to prepay::Model
 * and calls into this.
 */

#include <cstdint>
#include <memory>
#include <span>

#include "prepay/prepay.h"  // prepay_pool_t, prepay_model_config_t, enums (POD only)

namespace prepay {

// Internal rate/economic environment; defined in scenario.hpp. Forward-declared
// here so model signatures stay in C++-type-land and this header doesn't drag
// in <vector>. The shim reinterprets prepay_scenario_t* -> prepay::Scenario*.
// Null for rate-independent models.
class Scenario;

// Abstract prepayment model.
//
// project_pool fills one pool's whole path, so there is exactly one virtual
// dispatch per pool per projection — never per month. Path-dependent state
// (burnout, media effect) can live on the stack inside the override without a
// virtual on the hot inner loop.
class Model {
public:
    explicit Model(std::uint32_t model_version) noexcept
        : model_version_(model_version) {}
    virtual ~Model() = default;

    Model(const Model&)            = delete;
    Model& operator=(const Model&) = delete;

    // Write SMM for one pool, months [0, out_smm.size()). Implementations must
    // populate every element. `scenario` may be null (see requires_scenario()).
    virtual void project_pool(const prepay_pool_t& pool,
                              const Scenario*      scenario,
                              std::span<double>    out_smm) const = 0;

    // True if project_pool dereferences `scenario`. Rate-independent models
    // return false; the shim rejects a null scenario for models that need one,
    // and additionally guarantees scenario->n_months() >= out_smm.size().
    virtual bool requires_scenario() const noexcept = 0;

    // Caller-stamped version, carried for output tagging / audit reproducibility.
    std::uint32_t model_version() const noexcept { return model_version_; }

private:
    std::uint32_t model_version_;
};

// Constant annual CPR -> flat SMM. Ignores age, rates, scenario.
class ConstCprModel final : public Model {
public:
    ConstCprModel(double annual_cpr, std::uint32_t model_version);  // cpr in [0,1]

    void project_pool(const prepay_pool_t& pool,
                      const Scenario*      scenario,
                      std::span<double>    out_smm) const override;
    bool requires_scenario() const noexcept override { return false; }

private:
    double smm_;  // precomputed 1 - (1 - annual_cpr)^(1/12)
};

// PSA ramp. 100 PSA: CPR ramps 0.2%/month to 6% at age 30, then flat. `speed`
// is the multiple (1.0 == 100 PSA). Uses pool.age + month offset; scenario
// ignored. Exact age/off-by-one convention is pinned in model.cpp.
class PsaModel final : public Model {
public:
    PsaModel(double speed, std::uint32_t model_version);  // speed >= 0

    void project_pool(const prepay_pool_t& pool,
                      const Scenario*      scenario,
                      std::span<double>    out_smm) const override;
    bool requires_scenario() const noexcept override { return false; }

private:
    double speed_;
};

// Refi S-curve (0.2.0). Annual CPR is logistic in the refi incentive
// (pool.wac - scenario.mortgage_rate[t]), damped by a linear seasoning ramp:
//
//     cpr(t) = season(t) * [floor + (ceiling - floor) * L(incentive(t))]
//     L(x)   = 1 / (1 + exp(-steepness * (x - midpoint)))
//
// Incentive is in annual decimal (0.01 == 100bp), so steepness is ~400, not ~4.
// Requires a scenario; the shim guarantees non-null and a long enough path.
//
// No burnout yet: this is a pure function of (pool, t), so two pools with the
// same wac and age get identical paths regardless of how much refi opportunity
// they have already been through. That is the next model change.
class RefiScurveModel final : public Model {
public:
    struct Params {
        double floor;             // baseline (turnover) CPR at zero incentive
        double ceiling;           // saturation CPR deep in-the-money
        double midpoint;          // incentive at half height
        double steepness;         // logistic slope
        double seasoning_months;  // ramp length; <= 1 disables the ramp
    };

    RefiScurveModel(const Params& params, std::uint32_t model_version);

    void project_pool(const prepay_pool_t& pool,
                      const Scenario*      scenario,
                      std::span<double>    out_smm) const override;
    bool requires_scenario() const noexcept override { return true; }

private:
    Params params_;
};

// Validate config and build the matching model. Throws std::invalid_argument on
// bad parameters (e.g. CPR out of range); the C ABI shim catches and maps to a
// prepay_status_t. struct_size validation happens at the boundary in the shim,
// which also normalizes short/long caller configs onto our layout before this
// sees them — so every field read here is one the shim vouched for.
std::unique_ptr<Model> make_model(const prepay_model_config_t& cfg);

}  // namespace prepay

#endif  // PREPAY_SRC_MODEL_HPP_INCLUDED
