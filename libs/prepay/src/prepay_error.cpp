#include "prepay/prepay.h"

#include "error.hpp"

#include <cstddef>
#include <cstdio>
#include <string>

namespace {

// Thread-local, fixed-size: zero dynamic init, no destructor, no allocation on
// the error path. Sized generously for our exception messages; longer messages
// are truncated. Valid until the next prepay_* call on the same thread.
constexpr std::size_t kErrBufSize = 512;
thread_local char g_last_error[kErrBufSize] = {0};

}  // namespace

namespace prepay {

void set_last_error(const char* msg) noexcept {
    if (msg == nullptr) {
        g_last_error[0] = '\0';
        return;
    }
    // snprintf always null-terminates and never overruns the buffer.
    std::snprintf(g_last_error, kErrBufSize, "%s", msg);
}

void set_last_error(const std::string& msg) noexcept {
    set_last_error(msg.c_str());
}

void clear_last_error() noexcept {
    g_last_error[0] = '\0';
}

}  // namespace prepay

extern "C" {

const char* prepay_last_error(void) {
    return g_last_error;
}

const char* prepay_status_str(prepay_status_t status) {
    switch (status) {
        case PREPAY_OK:              return "ok";
        case PREPAY_ERR_NULL_ARG:    return "null argument";
        case PREPAY_ERR_INVALID_ARG: return "invalid argument";
        case PREPAY_ERR_BUFFER_SIZE: return "output buffer size mismatch";
        case PREPAY_ERR_UNSUPPORTED: return "unsupported operation";
        case PREPAY_ERR_ALLOC:       return "allocation failure";
        case PREPAY_ERR_INTERNAL:    return "internal error";
    }
    return "unknown status";
}

}  // extern "C"
