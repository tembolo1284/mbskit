#ifndef PREPAY_SRC_SCENARIO_HPP_INCLUDED
#define PREPAY_SRC_SCENARIO_HPP_INCLUDED

/*
 * Private rate/economic environment. The C ABI hands out prepay_scenario_t*,
 * which is this class reinterpret_cast (struct prepay_scenario is never defined).
 *
 * Ownership: the rate path is COPIED at construction. The caller's buffer is
 * dead to us the moment the constructor returns — that is what lets the Python
 * wrapper pass a temporary numpy array without lifetime bookkeeping.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace prepay {

class Scenario {
public:
    // Throws std::invalid_argument on an empty path or a non-finite /
    // implausible rate (message carries the offending index).
    Scenario(std::span<const double> mortgage_rate, std::uint32_t scenario_id);

    // Precondition: month < n_months(). Callers get that guarantee from the
    // shim's SHORT_PATH check, so the hot loop stays branch-free.
    double mortgage_rate(std::size_t month) const noexcept {
        return rate_[month];
    }

    std::size_t   n_months()    const noexcept { return rate_.size(); }
    std::uint32_t scenario_id() const noexcept { return scenario_id_; }

private:
    std::vector<double> rate_;
    std::uint32_t       scenario_id_;
};

}  // namespace prepay

#endif  // PREPAY_SRC_SCENARIO_HPP_INCLUDED
