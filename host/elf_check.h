// SPDX-License-Identifier: MIT
// Checks a converted game file (an AArch64 ELF) before the loader maps it, so an empty, cut-off or wrong file gives a plain message
// instead of a crash (project code, ours). No dependencies: the launcher, the loader and the unit test all use it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace elf_check {

// True when the bytes can be mapped by loader::load. On false, `err` is a plain phrase that continues "The file ..."
// ("is empty", "is cut off: ...").
bool check_bytes(const uint8_t* data, size_t size, std::string& err);

// Reads the file and checks it. `label` ("main" or "sdk") starts the message: "The main file is empty. Run the builder again to convert the game."
bool check_file(const char* path, const char* label, std::string& err);

}  // namespace elf_check
