#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>

#include "prepay/prepay.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

// Map a C ABI status onto the C++ exception nanobind translates to the right
// Python type: invalid_argument -> ValueError, runtime_error -> RuntimeError,
// bad_alloc -> MemoryError. The last-error detail carries the specifics
// (offending row index, parameter name, actual value).
[[noreturn]] void raise_status(prepay_status_t s, const char* where) {
    std::string msg = std::string(where) + ": " + prepay_status_str(s);
    const char* detail = prepay_last_error();
    if (detail != nullptr && *detail != '\0') {
        msg += " (";
        msg += detail;
        msg += ")";
    }
    switch (s) {
        case PREPAY_ERR_NULL_ARG:
        case PREPAY_ERR_INVALID_ARG:
        case PREPAY_ERR_BUFFER_SIZE:
        case PREPAY_ERR_SHORT_PATH:
            throw std::invalid_argument(msg);
        case PREPAY_ERR_ALLOC:
            throw std::bad_alloc();
        default:  // UNSUPPORTED, INTERNAL
            throw std::runtime_error(msg);
    }
}

// 1-D, C-contiguous, CPU input arrays of exact dtype (the Python wrapper
// coerces before calling, so these stay zero-copy).
using U32Arr = nb::ndarray<const std::uint32_t, nb::ndim<1>, nb::c_contig, nb::device::cpu>;
using F64Arr = nb::ndarray<const double,        nb::ndim<1>, nb::c_contig, nb::device::cpu>;

// RAII owner for a prepay_model_t handle. Construction validates via the C ABI
// and raises on failure; destruction frees the handle.
class Model {
public:
    // CONST_CPR / PSA: a single `param`.
    Model(prepay_model_type_t type, double param, std::uint32_t model_version) {
        prepay_model_config_t cfg{};
        cfg.struct_size   = sizeof(cfg);
        cfg.model_version = model_version;
        cfg.type          = type;
        cfg.param         = param;
        create(cfg);
    }

    // REFI_SCURVE: the five appended parameters. Distinct arity from the
    // overload above, so no ambiguity at the binding layer.
    Model(double floor, double ceiling, double midpoint, double steepness,
          double seasoning_months, std::uint32_t model_version) {
        prepay_model_config_t cfg{};
        cfg.struct_size             = sizeof(cfg);
        cfg.model_version           = model_version;
        cfg.type                    = PREPAY_MODEL_REFI_SCURVE;
        cfg.scurve_floor            = floor;
        cfg.scurve_ceiling          = ceiling;
        cfg.scurve_midpoint         = midpoint;
        cfg.scurve_steepness        = steepness;
        cfg.scurve_seasoning_months = seasoning_months;
        create(cfg);
    }

    ~Model() { prepay_model_destroy(handle_); }

    Model(const Model&)            = delete;
    Model& operator=(const Model&) = delete;

    const prepay_model_t* handle() const noexcept { return handle_; }

    std::uint32_t model_version() const {
        std::uint32_t v = 0;
        prepay_status_t s = prepay_model_version(handle_, &v);
        if (s != PREPAY_OK) raise_status(s, "model_version");
        return v;
    }

private:
    void create(const prepay_model_config_t& cfg) {
        prepay_status_t s = prepay_model_create(&cfg, &handle_);
        if (s != PREPAY_OK) raise_status(s, "model_create");
    }

    prepay_model_t* handle_ = nullptr;
};

// RAII owner for a prepay_scenario_t handle.
//
// The C ABI copies the rate path on create, so the numpy array the caller
// passed is free to die immediately — no lifetime bookkeeping here. close()
// exists for deterministic release (and __exit__); the destructor is the
// backstop for a scenario that just goes out of scope. Both are safe to call
// repeatedly: prepay_scenario_destroy is NULL-safe and we null the handle.
class Scenario {
public:
    Scenario(F64Arr mortgage_rate, std::uint32_t scenario_id) {
        prepay_scenario_config_t cfg{};
        cfg.struct_size   = sizeof(cfg);
        cfg.scenario_id   = scenario_id;
        cfg.mortgage_rate = mortgage_rate.data();
        cfg.n_months      = mortgage_rate.shape(0);

        prepay_status_t s = prepay_scenario_create(&cfg, &handle_);
        if (s != PREPAY_OK) raise_status(s, "scenario_create");
        n_months_ = cfg.n_months;
    }

    ~Scenario() { close(); }

    Scenario(const Scenario&)            = delete;
    Scenario& operator=(const Scenario&) = delete;

    void close() noexcept {
        prepay_scenario_destroy(handle_);
        handle_ = nullptr;
    }

    bool closed() const noexcept { return handle_ == nullptr; }

    const prepay_scenario_t* handle() const noexcept { return handle_; }

    std::size_t n_months() const noexcept { return n_months_; }

    std::uint32_t scenario_id() const {
        if (handle_ == nullptr) {
            throw std::runtime_error("scenario_id: scenario is closed");
        }
        std::uint32_t id = 0;
        prepay_status_t s = prepay_scenario_id(handle_, &id);
        if (s != PREPAY_OK) raise_status(s, "scenario_id");
        return id;
    }

private:
    prepay_scenario_t* handle_   = nullptr;
    std::size_t        n_months_ = 0;
};

nb::ndarray<nb::numpy, double> project(const Model&    model,
                                       const Scenario* scenario,
                                       U32Arr          original_term,
                                       U32Arr          age,
                                       F64Arr          balance,
                                       F64Arr          wac,
                                       F64Arr          note_rate,
                                       std::size_t     horizon) {
    const std::size_t n = age.shape(0);
    if (original_term.shape(0) != n || balance.shape(0) != n ||
        wac.shape(0) != n || note_rate.shape(0) != n) {
        throw std::invalid_argument("pool field arrays must have equal length");
    }
    if (n == 0 || horizon == 0) {
        throw std::invalid_argument("n_pools and horizon must be > 0");
    }
    if (scenario != nullptr && scenario->closed()) {
        throw std::runtime_error("project: scenario is closed");
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

    const prepay_scenario_t* scen =
        (scenario != nullptr) ? scenario->handle() : nullptr;

    prepay_status_t s;
    {
        // The compute is pure native work — let other Python threads run.
        nb::gil_scoped_release release;
        s = prepay_project(model.handle(), scen,
                           pools.data(), n, horizon,
                           data, total);
    }
    if (s != PREPAY_OK) {
        // `owner` frees `data` as it goes out of scope.
        raise_status(s, "project");
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
        .value("CONST_CPR",   PREPAY_MODEL_CONST_CPR)
        .value("PSA",         PREPAY_MODEL_PSA)
        .value("REFI_SCURVE", PREPAY_MODEL_REFI_SCURVE);

    nb::class_<Model>(m, "Model")
        .def(nb::init<prepay_model_type_t, double, std::uint32_t>(),
             "type"_a, "param"_a, "model_version"_a = 0)
        .def(nb::init<double, double, double, double, double, std::uint32_t>(),
             "floor"_a, "ceiling"_a, "midpoint"_a, "steepness"_a,
             "seasoning_months"_a, "model_version"_a = 0,
             "Build a REFI_SCURVE model from its five parameters.")
        .def_prop_ro("model_version", &Model::model_version,
                     "The caller-stamped version carried for audit.");

    nb::class_<Scenario>(m, "Scenario")
        .def(nb::init<F64Arr, std::uint32_t>(),
             "mortgage_rate"_a, "scenario_id"_a = 0,
             "Rate environment. The path is copied; the array may be freed.")
        .def("close", &Scenario::close,
             "Release the native handle now. Idempotent.")
        .def("__enter__", [](Scenario& s) -> Scenario& { return s; },
             nb::rv_policy::reference_internal)
        .def("__exit__",
             [](Scenario& s, nb::object, nb::object, nb::object) {
                 s.close();
                 return false;  // never suppress an exception
             },
	     nb::arg("exc_type").none(),
	     nb::arg("exc_value").none(),
	     nb::arg("traceback").none())
        .def_prop_ro("closed", &Scenario::closed)
        .def_prop_ro("n_months", &Scenario::n_months)
        .def_prop_ro("scenario_id", &Scenario::scenario_id,
                     "The caller-stamped id carried for audit.");

    m.def("project", &project,
          "model"_a, "scenario"_a.none(), "original_term"_a, "age"_a,
          "balance"_a, "wac"_a, "note_rate"_a, "horizon"_a,
          "Project SMM for each pool over `horizon` months; returns "
          "an (n_pools, horizon) float64 array.");
}
