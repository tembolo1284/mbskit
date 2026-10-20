#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>

#include "prepay/prepay.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// RAII owner for a prepay_model_t handle. Construction validates via the C ABI
// and raises on failure; destruction frees the handle.
class Model {
public:
    Model(prepay_model_type_t type, double param, std::uint32_t model_version) {
        prepay_model_config_t cfg{};
        cfg.struct_size   = sizeof(cfg);
        cfg.model_version = model_version;
        cfg.type          = type;
        cfg.param         = param;

        prepay_status_t s = prepay_model_create(&cfg, &handle_);
        if (s != PREPAY_OK) {
            throw std::invalid_argument(prepay_last_error());
        }
    }
    ~Model() { prepay_model_destroy(handle_); }

    Model(const Model&)            = delete;
    Model& operator=(const Model&) = delete;

    const prepay_model_t* handle() const noexcept { return handle_; }

private:
    prepay_model_t* handle_ = nullptr;
};

// 1-D, C-contiguous, CPU input arrays of exact dtype (the Python wrapper
// coerces before calling, so these stay zero-copy).
using U32Arr = nb::ndarray<const std::uint32_t, nb::ndim<1>, nb::c_contig, nb::device::cpu>;
using F64Arr = nb::ndarray<const double,        nb::ndim<1>, nb::c_contig, nb::device::cpu>;

nb::ndarray<nb::numpy, double> project(const Model& model,
                                       U32Arr       original_term,
                                       U32Arr       age,
                                       F64Arr       balance,
                                       F64Arr       wac,
                                       F64Arr       note_rate,
                                       std::size_t  horizon) {
    const std::size_t n = age.shape(0);
    if (original_term.shape(0) != n || balance.shape(0) != n ||
        wac.shape(0) != n || note_rate.shape(0) != n) {
        throw std::invalid_argument("pool field arrays must have equal length");
    }
    if (n == 0 || horizon == 0) {
        throw std::invalid_argument("n_pools and horizon must be > 0");
    }

    // Pack the columnar inputs into the POD row layout the C ABI expects.
    std::vector<prepay_pool_t> pools(n);
    const std::uint32_t* ot  = original_term.data();
    const std::uint32_t* ag  = age.data();
    const double*        bal = balance.data();
    const double*        w   = wac.data();
    const double*        nr  = note_rate.data();
    for (std::size_t i = 0; i < n; ++i) {
        prepay_pool_t& p = pools[i];
        p.struct_size   = sizeof(prepay_pool_t);
        p.original_term = ot[i];
        p.age           = ag[i];
        p.balance       = bal[i];
        p.wac           = w[i];
        p.note_rate     = nr[i];
    }

    // Output buffer owned by a capsule so numpy frees it when the array dies.
    const std::size_t total = n * horizon;
    double* data = new double[total];
    nb::capsule owner(data, [](void* p) noexcept {
        delete[] static_cast<double*>(p);
    });

    prepay_status_t s;
    {
        // The compute is pure native work — let other Python threads run.
        nb::gil_scoped_release release;
        s = prepay_project(model.handle(), nullptr,
                           pools.data(), n, horizon,
                           data, total);
    }
    if (s != PREPAY_OK) {
        // `owner` frees `data` as it goes out of scope.
        throw std::runtime_error(std::string(prepay_status_str(s)) + ": " +
                                 prepay_last_error());
    }

    const std::size_t shape[2] = {n, horizon};
    return nb::ndarray<nb::numpy, double>(data, 2, shape, owner);
}

}  // namespace

NB_MODULE(_prepay, m) {
    m.doc() = "Low-level nanobind bindings for the prepay C ABI";

    m.attr("__version__") =
        std::to_string(PREPAY_VERSION_MAJOR) + "." +
        std::to_string(PREPAY_VERSION_MINOR) + "." +
        std::to_string(PREPAY_VERSION_PATCH);

    m.def("version", [] { return (std::uint32_t)prepay_get_version(); },
          "Runtime version of the linked libprepay, encoded major<<16|minor<<8|patch.");

    nb::enum_<prepay_model_type_t>(m, "ModelType")
        .value("CONST_CPR", PREPAY_MODEL_CONST_CPR)
        .value("PSA",       PREPAY_MODEL_PSA);

    nb::class_<Model>(m, "Model")
        .def(nb::init<prepay_model_type_t, double, std::uint32_t>(),
             "type"_a, "param"_a, "model_version"_a = 0);

    m.def("project", &project,
          "model"_a, "original_term"_a, "age"_a, "balance"_a,
          "wac"_a, "note_rate"_a, "horizon"_a,
          "Project SMM for each pool over `horizon` months; returns "
          "an (n_pools, horizon) float64 array.");
}
