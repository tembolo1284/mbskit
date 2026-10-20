#ifndef PREPAY_SRC_ERROR_HPP_INCLUDED
#define PREPAY_SRC_ERROR_HPP_INCLUDED

/*
 * Internal error-reporting hooks. The extern "C" shim calls set_last_error()
 * from its catch blocks; prepay_last_error() (public C ABI) reads the same
 * thread-local buffer. Not exported.
 */

#include <string>

namespace prepay {

// Copy `msg` into the calling thread's last-error buffer (truncated to fit,
// always null-terminated). A null pointer clears the buffer.
void set_last_error(const char* msg) noexcept;
void set_last_error(const std::string& msg) noexcept;

// Clear the calling thread's last-error buffer.
void clear_last_error() noexcept;

}  // namespace prepay

#endif  // PREPAY_SRC_ERROR_HPP_INCLUDED
