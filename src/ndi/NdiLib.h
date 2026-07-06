#pragma once

// Central include + lifecycle for the NDI SDK. Everything NDI-facing goes
// through this header so the SDK surface stays in one place.
// The SDK headers use NULL/int64_t without including <cstddef>/<cstdint>
// themselves (breaks on gcc >= 15); provide them first.
#include <cstddef>
#include <cstdint>

#include <Processing.NDI.Lib.h>

namespace moo::ndi {

// NDIlib_initialize + version log. Returns false if the CPU/SDK refuses.
bool initialize();
void destroy();

}  // namespace moo::ndi
