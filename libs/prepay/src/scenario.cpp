#include "scenario.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace prepay {

Scenario::Scenario(std::span<const double> mortgage_rate, std::uint32_t scenario_id)
    : rate_(mortgage_rate.begin(), mortgage_rate.end()),
      scenario_id_(scenario_id) {
    if (rate_.empty()) {
        throw std::invalid_argument("scenario: mortgage_rate path is empty");
    }
    // Annual decimal. Negative nominal mortgage rates aren't something we model,
    // and anything above 100% is a units error (bps passed where decimal belongs).
    for (std::size_t i = 0; i < rate_.size(); ++i) {
        const double r = rate_[i];
        if (!std::isfinite(r) || r < 0.0 || r > 1.0) {
            throw std::invalid_argument(
                "scenario: mortgage_rate[" + std::to_string(i) +
                "] must be finite and in [0, 1] (annual decimal), got " +
                std::to_string(r));
        }
    }
}

}  // namespace prepay
