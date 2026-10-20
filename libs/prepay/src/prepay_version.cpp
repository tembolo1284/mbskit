#include "prepay/prepay.h"

/* prepay_abi_compatible() is defined inline in the public header so it captures
 * the caller's compile-time version. Only the runtime version lives here. */

extern "C" {

uint32_t prepay_get_version(void) {
    return (uint32_t)PREPAY_VERSION;
}

}  // extern "C"
