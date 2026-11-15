// SPDX-License-Identifier: MIT
// HD photos (SNAP_HD_PHOTOS=1, settings key hd_photos; off by default). With a render scale or upscaler the scene the game
// photographs is larger than its 1080x720 photo and 360x240 thumbnail. Each capture draw also blits the same frame region into a
// host-only 2160x1440 image; photo select's large preview and the album's full view then sample that image instead of the
// game's small texture. The guest never sees the HD pixels: its own photo, thumbnail, JPEG, save and score are unchanged.
// Saved photos keep their HD copy as <save root>\photos_hd\<album JPEG CRC32>.jpg (q95). Details: analysis/core/hd_photos.md.
#pragma once
#include <cstdint>
namespace nvn {
struct ReplayCtx; struct ProgramImpl; struct TextureImpl;
bool HdPhotoOn();   // SNAP_HD_PHOTOS, read once
// A draw with one sampled texture into a 360x240 or 1080x720 target: when it is the game's capture pass (CapturePS /
// CaptureLanczos3PS) on a large enough source, records the 2160x1440 blit and its readback. c4 = the pass's {add, multiply};
// uv = the sampled source rect {u0, v0, u1, v1} (0..1) from the draw's vertices, or nullptr (then the centered 3:2 crop).
void HdPhotoCapture(ReplayCtx& cx, ProgramImpl* p, TextureImpl* src, TextureImpl* rt0, const float* c4, const float* uv);
void HdPhotoCompress(const TextureImpl* bc, const TextureImpl* src);   // thumbnail BC1 pass: bc shows what src shows
void HdPhotoCopyTex(const TextureImpl* s, const TextureImpl* d);       // CopyTextureToTexture
// CopyTextureToTexture into a 1080x720 texture (guest source region): the photo made at the shutter from the display texture.
void HdPhotoCopyRegion(ReplayCtx& cx, TextureImpl* s, TextureImpl* d, int sx, int sy, int sw, int sh);
void HdPhotoCopyToBuffer(const TextureImpl* t, const uint8_t* dst);    // CopyTextureToBuffer (the album photo before JpegEncoder)
void HdPhotoUploadFrom(const TextureImpl* t, const uint8_t* src);      // CopyBufferToTexture (src = host pointer, may be null)
void HdPhotoTexGone(const TextureImpl* t);
void HdPhotoReadback(uint64_t id, const uint8_t* map, uint64_t size);  // FlushReadbacks, readback kind 4
// The HD texture to sample instead of `t`, or nullptr. drawnW = on-screen width of the sprite in 1920-wide UI units (0 = unknown).
TextureImpl* HdPhotoSubstitute(TextureImpl* t, float drawnW);
}  // namespace nvn
