// SPDX-License-Identifier: MIT
// HDR photo copies (SNAP_HDR_PHOTOS=1, settings key hdr_photos, CLI --hdr-photos on|off; off by default). When the game runs with HDR output
// (SNAP_HDR=hdr10|scrgb, active swapchain), each HD photo copy and each screenshot is also written as <name>.hdr.png: 16-bit PNG, PQ transfer,
// BT.2020 primaries, with cICP, mDCv and cLLi chunks. It RECONSTRUCTS highlights from the 8-bit picture with the HDR present pass's inverse tone
// map; it does not capture the scene before tonemap. With HDR output off nothing is written. Details: docs/hdr-photos.md.
#pragma once
#include <cstdint>
#include <string>

namespace nvn {
bool HdrPhotoWanted();   // SNAP_HDR_PHOTOS / hdr_photos
// True when wanted and an HDR swapchain is active; logs the reason once per change when wanted but HDR output is off.
bool HdrPhotoEnabled();
// px: rows of w*4 bytes, display-encoded sRGB (RGBA, or BGRA when bgra). Writes <path> (temp file, then rename). Call from a worker thread.
bool HdrPhotoWrite(const std::string& path, const uint8_t* px, uint32_t w, uint32_t h, bool bgra);
// "<base>.hdr.png" for a ".jpg" / ".png" path.
std::string HdrPhotoPathFor(const std::string& path);
}  // namespace nvn
