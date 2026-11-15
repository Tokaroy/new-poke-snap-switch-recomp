// SPDX-License-Identifier: MIT
// Optional hook: hid.cpp calls it when the A button is down (the photo shutter). Null unless the NVN pipeline service
// registers one, so the HLE test binary links without nvn_pipeline.cpp.
#pragma once

namespace hle {
inline void (*g_shutter_hook)() = nullptr;
}  // namespace hle
