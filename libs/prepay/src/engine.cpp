#include "engine.hpp"

#include <cassert>
#include <cstddef>

namespace prepay {

void project(const Model&                   model,
             const Scenario*                scenario,
             std::span<const prepay_pool_t> pools,
             std::size_t                    horizon,
             std::span<double>              out_smm) {
    assert(out_smm.size() == pools.size() * horizon &&
           "engine::project: out_smm must be n_pools * horizon");

    for (std::size_t i = 0; i < pools.size(); ++i) {
        // Each pool gets its own contiguous row; project_pool may carry
        // path-dependent state (burnout, etc.) on the stack within the call.
        const std::span<double> row = out_smm.subspan(i * horizon, horizon);
        model.project_pool(pools[i], scenario, row);
    }
}

}  // namespace prepay
