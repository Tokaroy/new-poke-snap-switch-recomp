// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see ../LICENSE.
#include "../cheat_vm.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Fake : cheat::Memory {
    std::array<uint8_t, 65536> bytes{};
    uint64_t keys = 0, logged = 0;
    unsigned reads = 0, writes = 0, pauses = 0, resumes = 0, log_id = 0, log_width = 0;
    bool read(uint64_t address, void* out, size_t n) noexcept override {
        ++reads; if (address > bytes.size() || n > bytes.size() - size_t(address)) return false;
        std::memcpy(out, bytes.data() + size_t(address), n); return true;
    }
    bool write(uint64_t address, const void* in, size_t n) noexcept override {
        ++writes; if (address > bytes.size() || n > bytes.size() - size_t(address)) return false;
        std::memcpy(bytes.data() + size_t(address), in, n); return true;
    }
    uint64_t heap_base() const noexcept override { return 0x2000; }
    uint64_t main_nso_base() const noexcept override { return 0x1000; }
    uint64_t alias_base() const noexcept override { return 0x3000; }
    uint64_t aslr_base() const noexcept override { return 0x4000; }
    uint64_t keys_held() const noexcept override { return keys; }
    bool pause() noexcept override { ++pauses; return true; }
    bool resume() noexcept override { ++resumes; return true; }
    void debug_log(unsigned id, uint64_t value, unsigned width) noexcept override { log_id = id; logged = value; log_width = width; }
    uint64_t at(size_t address, unsigned width = 4) const {
        uint64_t result = 0; for (unsigned i = 0; i < width; ++i) result |= uint64_t(bytes[address + i]) << (8 * i); return result;
    }
};
cheat::Vm make(const std::string& text, bool enable = true) {
    cheat::Program p; cheat::Error e;
    const bool ok = cheat::parse(text, p, e);
    if (!ok) std::fprintf(stderr, "parse line %zu: %s\n", e.line, e.message.c_str());
    require(ok, "synthetic test program did not parse");
    cheat::Vm vm(std::move(p));
    if (enable) for (size_t s = 0; s < vm.program().sections.size(); ++s) require(vm.enable(s, true), "enable failed");
    return vm;
}
std::string word(uint32_t value) { char b[16]; std::snprintf(b, sizeof b, "%08X ", value); return b; }
uint32_t op(std::initializer_list<unsigned> nibbles) { uint32_t out = 0; for (unsigned n : nibbles) out = (out << 4) | n; return out; }
void memory_and_registers() {
    Fake m;
    auto vm = make("{master}\n40010000 00000000 00000010\n[test]\n"
                   "04010000 00000020 12345678\n54020000 00000030\n"
                   "40030000 00000000 00002000\n64031000 00000000 DEADBEEF\n"
                   "74020000 00000001\n94042010\nA4210400 00000040\n"
                   "C1070210\nC1090700\nC1090030\nC2100004\nC2300004\nC2000004\n"
                   "C3000852\nC3000056\nFFF47420\nFF000000\nFF100000\n");
    const auto r = vm.run(m);
    require(r.ok, "memory/register program failed");
    require(m.at(0x1030) == 0x12345678, "type 0 register-relative store differs");
    require(m.at(0x2000) == 0xdeadbeef && vm.registers()[3] == 0x2004, "type 6 store/increment differs");
    require(m.at(0x1040) == 0x12345679 && vm.registers()[4] == 0x12345689, "type 7/9/A arithmetic or store differs");
    require(vm.registers()[9] == 0 && vm.registers()[2] == 0x12345679 && vm.registers()[6] == vm.registers()[2], "C1/C2/C3 differs");
    require(m.log_id == 7 && m.logged == vm.registers()[2] && m.log_width == 4, "FFF register debug log differs");
    require(m.pauses == 1 && m.resumes == 1, "FF0/FF1 callbacks differ");
    auto disabled = make("{always}\n04000000 00000004 00000001\n[optional]\n04000000 00000008 00000002\n", false);
    Fake other; require(disabled.run(other).ok && other.at(0x1004) == 1 && other.at(0x1008) == 0, "master/enable semantics differ");
}
void conditions_and_loops() {
    Fake m;
    auto vm = make("[test]\n04000000 00000010 00000002\n"
                   "14050000 00000010 00000002\n80000003\nC0450400 00000000\n"
                   "04000000 00000020 000000AA\n20000000\n21000000\n"
                   "04000000 00000020 000000BB\n20000000\n21000000\n"
                   "04000000 00000020 000000CC\n20000000\n"
                   "30040000 00000003\n74020000 00000001\n31040000\n"
                   "30050000 00000000\n04000000 00000030 000000FF\n31050000\n");
    m.keys = 1; require(vm.run(m).ok && m.at(0x1020) == 0xbb, "nested keypress else differs");
    m.keys = 3; require(vm.run(m).ok && m.at(0x1020) == 0xaa, "all key-mask bits must match");
    require(vm.registers()[2] == 3 && vm.registers()[4] == 0 && m.at(0x1030) == 0, "loop count or zero-loop behavior differs");
    auto edge = make("[edge]\nC4000000 00000001 00000000\n04000000 00000000 00000001\n20000000\n");
    Fake e; e.keys = uint64_t(1) << 32;
    require(edge.run(e).ok && e.writes == 1, "C4 high-bit first press failed");
    require(edge.run(e).ok && e.writes == 1, "C4 repeated held press was not suppressed");
    e.keys = 0; require(edge.run(e).ok, "C4 release failed");
    e.keys = uint64_t(1) << 32; require(edge.run(e).ok && e.writes == 2, "C4 did not rearm");
    auto repeat = make("[repeat]\nC4100000 00000000 00000001\n04000000 00000000 00000002\n20000000\n");
    e.keys = 1; unsigned before = e.writes;
    require(repeat.run(e).ok && repeat.run(e).ok && e.writes == before + 2, "C4 repeat mode differs");
}
void variants() {
    // Every arithmetic operation at width 4; width-specific memory cases follow.
    for (unsigned operation = 0; operation < 14; ++operation) {
        const bool floating = operation >= 10;
        std::string text = "[arithmetic]\n40010000 00000000 " + word(floating ? 0x3fc00000 : 6) + "\n40020000 00000000 " + word(floating ? 0x40000000 : 2) + "\n";
        text += word(op({9, 4, operation, 3, 1, 0, 2, 0})) + "\n";
        auto vm = make(text); Fake m; require(vm.run(m).ok, "arithmetic variant failed");
        if (!floating) {
            const uint64_t expected[] = {8, 4, 12, 24, 1, 2, 6, 0xfffffff9u, 4, 6};
            require(vm.registers()[3] == expected[operation], "integer arithmetic result differs");
        } else {
            const float expected[] = {3.5f, -0.5f, 3.0f, 0.75f};
            uint32_t bits; std::memcpy(&bits, &expected[operation - 10], 4);
            require(vm.registers()[3] == bits, "float arithmetic result differs");
        }
    }
    for (unsigned region = 0; region <= 4; ++region) for (unsigned width : {1u, 2u, 4u, 8u}) {
        std::string text = "[regions]\n" + word(op({0, width, region, 0, 0, 0, 0, 0})) + word(0x80) +
                           (width == 8 ? word(0x11223344) + word(0x55667788) : word(0x55667788)) + "\n";
        auto vm = make(text); Fake m; require(vm.run(m).ok, "region/width store failed");
        const size_t address = (region == 4 ? 0 : (region + 1) * 0x1000) + 0x80;
        uint64_t expected = width == 8 ? UINT64_C(0x1122334455667788) : UINT64_C(0x55667788) & ((UINT64_C(1) << (8 * width)) - 1);
        require(m.at(address, width) == expected, "region/width store differs");
    }
    auto data = make("[immediate]\n40010000 00000000 20000000\n"); Fake d;
    require(data.run(d).ok && data.registers()[1] == 0x20000000, "immediate was mistaken for a block terminator");
    auto pointer = make("[pointer]\n40010000 00000000 00001080\n54011000 00000000\n");
    Fake p; p.bytes[0x1080] = 0x37; require(pointer.run(p).ok && pointer.registers()[1] == 0x37, "type 5 pointer mode differs");
    auto save_clear = make("[clear]\n40010000 00000000 00000009\nC1010110\nC1010020\nC1010100\nC2100002\nC2200002\nC2000002\n");
    Fake s; require(save_clear.run(s).ok && save_clear.registers()[1] == 0, "saved-register clear variants differ");
    auto persistent = make("[static]\nC3000001\n74010000 00000001\nC3000801\n");
    require(persistent.run(s).ok && persistent.run(s).ok && persistent.registers()[1] == 2, "static registers did not persist across runs");
    persistent.reset(); require(persistent.run(s).ok && persistent.registers()[1] == 1, "static reset differs");
    auto doubles = make("[double]\n40010000 3FF80000 00000000\n40020000 40000000 00000000\n98A31020\n");
    require(doubles.run(s).ok, "double arithmetic failed");
    const double expected_double = 3.5; uint64_t double_bits;
    std::memcpy(&double_bits, &expected_double, 8);
    require(doubles.registers()[3] == double_bits, "double addition result differs");
    auto narrow = make("[narrow]\n40010000 00000000 000000FF\n91021100 00000002\n91332100 00000008\n");
    require(narrow.run(s).ok && narrow.registers()[2] == 1 && narrow.registers()[3] == 0, "narrow immediate arithmetic or oversized shift differs");
}
void address_modes() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        const std::string setup = "[load]\n40010000 00000000 00001000\n40020000 00000000 00000080\n40030000 00000000 00001000\n";
        const unsigned offset = mode == 3 ? 0 : 0x80;
        auto vm = make(setup + word(op({5, 4, 0, 3, mode, mode == 2 ? 1u : mode == 3 ? 2u : 0u, 0, 0})) + word(offset));
        Fake m; m.bytes[0x1080] = 99;
        require(vm.run(m).ok && vm.registers()[3] == 99, "type 5 addressing mode differs");
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        const unsigned r = mode == 0 ? 0x1080 : mode <= 2 ? 0x1000 : mode == 3 ? 0x80 : mode == 5 ? 0x40 : 0;
        std::string text = "[store]\n40010000 00000000 " + word(r) + "\n40020000 00000000 00000080\n40040000 00000000 00000063\n";
        text += word(op({0xa, 4, 4, 1, 0, mode, mode == 1 ? 2u : 0u, 0}));
        if (mode == 2 || mode == 4 || mode == 5) text += word(mode == 5 ? 0x40 : 0x80);
        auto vm = make(text); Fake m;
        require(vm.run(m).ok && m.at(0x1080) == 99, "type A addressing mode differs");
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        std::string text = "[condition]\n40010000 00000000 00001000\n40020000 00000000 00000080\n40030000 00000000 00000063\n40040000 00000000 00000063\n";
        const unsigned x = mode == 0 || mode == 1 || mode == 4 ? 0 : mode == 5 ? 3 : 1;
        const unsigned y = mode == 1 || mode == 3 ? 2 : 0;
        text += word(op({0xc, 0, 4, 5, 4, mode, x, y}));
        if (mode == 0 || mode == 2 || mode == 4) text += word(mode == 4 ? 99 : 0x80);
        text += "\n04000000 00000090 000000A5\n20000000\n";
        auto vm = make(text); Fake m; m.bytes[0x1080] = 99;
        require(vm.run(m).ok && m.at(0x1090) == 0xa5, "C0 addressing mode differs");
    }
    for (unsigned mode = 0; mode < 5; ++mode) {
        std::string text = "[log]\n40010000 00000000 00001000\n40020000 00000000 00000080\n40030000 00000000 00000063\n";
        const unsigned x = mode < 2 ? 0 : mode == 4 ? 3 : 1;
        const unsigned y = mode == 1 || mode == 3 ? 2 : 0;
        text += word(op({0xf, 0xf, 0xf, 4, 7, mode, x, y}));
        if (mode == 0 || mode == 2) text += word(0x80);
        auto vm = make(text); Fake m; m.bytes[0x1080] = 99;
        require(vm.run(m).ok && m.logged == 99 && m.log_id == 7, "FFF addressing mode differs");
    }
}
void public_program_and_limits() {
    cheat::Program p; cheat::Error e;
    require(cheat::parse("[test]\n40010000 00000000 00000001\n", p, e), "program fixture failed");
    p.sections[0].code[0].count = 4;
    bool rejected = false;
    try { cheat::Vm vm(p); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "forged public instruction count accepted");
    cheat::ParseLimits limits; limits.max_words = 2;
    require(!cheat::parse("[test]\n40010000 00000000 00000001\n", p, e, limits), "word limit ignored");
    limits = {}; limits.max_depth = 0;
    require(!cheat::parse("[test]\n80000001\n20000000\n", p, e, limits), "depth limit ignored");
    auto vm = make("[target]\n80000001\n04000000 00000000 00000001\n20000000\n");
    auto program = vm.program(); program.sections[0].code[0].target = size_t(-1);
    cheat::Vm rebuilt(std::move(program)); Fake m;
    require(rebuilt.run(m).ok && m.writes == 0, "constructor trusted a forged block target");
}
void failures() {
    for (const char* text : {"12345678\n", "[test]\n04000000\n", "[test]\nB0000000\n", "[test]\nC5000000\n",
                            "[test]\n20000000\n", "[test]\n80000001\n", "[test]\n30010000 00000002\n31020000\n",
                            "[test]\n04000000 00000000 NOTHEX00\n", "[broken\n", "[test]\n92A01000 00000000\n"}) {
        cheat::Program p; cheat::Error e;
        require(!cheat::parse(text, p, e) && e.line > 0 && !e.message.empty(), "malformed input was accepted without line error");
    }
    auto budget = make("[budget]\nFF000000\n30010000 FFFFFFFF\n74000000 00000001\n31010000\n");
    Fake m; const auto stopped = budget.run(m, 10);
    require(!stopped.ok && stopped.steps == 10 && m.pauses == 1 && m.resumes == 1, "budget or error-path resume differs");
    auto outside = make("[outside]\n40010000 FFFFFFFF FFFFFFFF\n64010000 00000000 00000001\n");
    Fake other; require(!outside.run(other).ok && other.writes == 0, "overflow address reached the memory callback");
    cheat::Program parsed; cheat::Error error;
    require(cheat::parse("\xef\xbb\xbf[test # name]\r\n40010000 00000000 00000001 // comment\r\n", parsed, error), "BOM/CRLF or section punctuation differs");
}
}  // namespace
int main() {
    memory_and_registers(); conditions_and_loops(); variants(); address_modes(); public_program_and_limits(); failures();
    std::printf("PASS: %u synthetic cheat VM checks\n", checks);
    return 0;
}
