// Guest static TLS image (project code, ours).
// The imported ELFs have no PT_TLS, so the runtime hands each guest thread a zeroed TLS block (tp in its middle). main's
// thread-locals are not all zero at start, though: its initialised TLS data (.tdata) is the 8 bytes at main+0xccb900, which the
// NSO to ELF import left inside a 16-byte ".data" section at 0xccb8f8 (8 bytes padding, then the image; nothing references it).
// AArch64 TLS variant 1: the first module's block starts right after the 16-byte TCB, at tp+0x10.
//   tp+0x10 (int32, image ffffffff = -1): the thread's slot in the physics world-bounds list (main+0xcfb1d8). AddBounds
//            (main+0x2a2970) appends a slot when it is negative. Zeroed, every thread "owns" slot 0 of an empty list, the list
//            stays empty, the broadphase grid (main+0x628850) spans a 6 cm box at the origin and no trigger collider ever pairs:
//            the camera scan (Sonar) never reaches a Pokemon, so no "?" and no name (the scan tutorial loops).
//   tp+0x14 (uint32, image 0x00732395): a hash the game overwrites before it reads it.
// The rest of main's TLS (up to tp+0x670) is .tbss and stays zero.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hle {

constexpr uint64_t kMainTlsImageVaddr = 0xccb900;   // main module offset of the .tdata image (2.0.1, v262144)
constexpr size_t kMainTlsImageSize = 8;
constexpr size_t kTlsBlockOffset = 0x10;            // first module's TLS block, after the 16-byte TCB

// Copy a module's TLS image into a fresh thread's block (tp = the guest thread pointer of a zeroed TLS area).
inline void guest_tls_init(uint8_t *tp, const uint8_t *image, size_t n) {
    if (tp && image && n) memcpy(tp + kTlsBlockOffset, image, n);
}

}  // namespace hle
