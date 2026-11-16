// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see LICENSE.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cheat {
struct Error { size_t line = 0; std::string message; };
struct Instruction {
    std::array<uint32_t, 4> words{};
    uint16_t type = 0;
    uint8_t count = 0;
    size_t line = 0, target = 0;
};
struct Section {
    std::string name;
    bool master = false, enabled = false;
    std::vector<Instruction> code;
};
struct Program { std::vector<Section> sections; };
struct ParseLimits { size_t max_words = 1u << 20, max_sections = 4096, max_depth = 64, max_bytes = 16u << 20; };
bool parse(std::string_view text, Program& output, Error& error, ParseLimits limits = {});

class Memory {
public:
    virtual ~Memory() = default;
    virtual bool read(uint64_t address, void* destination, size_t size) noexcept = 0;
    virtual bool write(uint64_t address, const void* source, size_t size) noexcept = 0;
    virtual uint64_t heap_base() const noexcept = 0;
    virtual uint64_t main_nso_base() const noexcept = 0;
    virtual uint64_t alias_base() const noexcept = 0;
    virtual uint64_t aslr_base() const noexcept = 0;
    virtual uint64_t keys_held() const noexcept = 0;
    virtual bool pause() noexcept = 0;
    virtual bool resume() noexcept = 0;
    virtual void debug_log(unsigned id, uint64_t value, unsigned width) noexcept { (void)id; (void)value; (void)width; }
};
struct Result { bool ok = true; size_t steps = 0, section = 0; Error error; };

class Vm {
public:
    explicit Vm(Program program);
    bool enable(size_t section, bool enabled);
    void reset();
    Result run(Memory& memory, size_t step_budget = 100000);
    const Program& program() const { return program_; }
    const std::array<uint64_t, 16>& registers() const { return registers_; }
private:
    Program program_;
    std::array<uint64_t, 16> registers_{}, saved_{};
    std::array<uint64_t, 128> static_{};
    std::vector<std::vector<uint8_t>> latches_;
};
}  // namespace cheat
