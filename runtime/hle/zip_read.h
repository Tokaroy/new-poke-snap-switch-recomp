// SPDX-License-Identifier: MIT
// Reading .zip files with no dependencies: the central directory (zip64 too), stored and deflate entries (RFC 1951, the
// same canonical-Huffman method as zlib's puff), checked against each entry's CRC32. Used by save import (save_port.cpp)
// and zipped mod packs (runtime/mods/mods_core.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hle {
namespace zipr {

struct Entry {
  std::string rel;          // '/' separated, as stored (backslashes turned into '/')
  uint64_t size = 0;        // uncompressed
  uint64_t csize = 0, lho = 0;
  uint16_t method = 0;      // 0 stored, 8 deflate
  uint32_t crc = 0;
};

uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n);
// The files of a zip (folders skipped). False with a user-facing message in *err for a damaged, encrypted or non-zip file.
bool list(const std::filesystem::path& zip, std::vector<Entry>& out, std::string* err);
// Writes entry `e` to `dst` (overwriting) and checks its size and CRC32.
bool extract(const std::filesystem::path& zip, const Entry& e, const std::filesystem::path& dst, std::string* err);

}  // namespace zipr
}  // namespace hle
