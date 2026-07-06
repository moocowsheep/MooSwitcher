#include "ndi/NdiLib.h"

#include "core/Log.h"

namespace moo::ndi {

bool initialize() {
    if (!NDIlib_initialize()) {
        MOO_LOGE("NDIlib_initialize failed (unsupported CPU?)");
        return false;
    }
    MOO_LOGI("NDI runtime: %s", NDIlib_version());
    return true;
}

void destroy() { NDIlib_destroy(); }

}  // namespace moo::ndi
