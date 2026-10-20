#ifndef PREPAY_SRC_ENGINE_HPP_INCLUDED
#define PREPAY_SRC_ENGINE_HPP_INCLUDED

/*
 * Private projection driver. Pure compute: it slices the output buffer into
 * per-pool rows and calls Model::project_pool. All C-ABI precondition checks
 * (null args, struct_size, buffer sizing, null-scenario-vs-requires) live in
 * the extern "C" shim, not here.
 */

#include <cstddef>
#include <span>

#include "model.hpp"  // Model, Scenario, prepay_pool_t

namespace prepay {

// Project SMM for every pool over `horizon` months. Row-major output:
//   out_smm[i * horizon + t]  for pool i, month t in [0, horizon).
//
// Precondition (guaranteed by the shim, asserted not re-validated here):
//   out_smm.size() == pools.size() * horizon.
//
// No allocation. `scenario` may be null when model.requires_scenario() is
// false. The per-pool loop is independent across pools, so a parallel backend
// (std::execution / thread pool) can drop in here unchanged.
void project(const Model&                   model,
             const Scenario*                scenario,
             std::span<const prepay_pool_t> pools,
             std::size_t                    horizon,
             std::span<double>              out_smm);

}  // namespace prepay

#endif  // PREPAY_SRC_ENGINE_HPP_INCLUDED
