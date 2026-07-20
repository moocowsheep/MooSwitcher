/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
