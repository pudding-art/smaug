#include "core/globals.h"
#include "operators/common.h"

namespace smaug {
void mapArrayToAccel(unsigned reqCode,
                     const char* arrayName,
                     void* baseAddr,
                     size_t size) {
    if (runningInSimulation) {
        mapArrayToAccelerator(reqCode, arrayName, baseAddr, size);
    }
}
}  // namespace smaug

#ifdef __cplusplus
extern "C" {
#endif

ALWAYS_INLINE
size_t next_multiple(size_t request, size_t align) {
    size_t n = request / align;
    if (n == 0)
        return align;  // Return at least this many bytes.
    size_t remainder = request % align;
    if (remainder)
        return (n + 1) * align;
    return request;
}

#ifdef __cplusplus
}
#endif