// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see LICENSE.
// Implemented from the documented instruction encodings, without implementation-source reuse.
#include "cheat_vm.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cheat {
namespace {
static_assert(sizeof(float) == 4 && sizeof(double) == 8 && std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<double>::is_iec559, "cheat float operations require IEEE binary32/binary64");
unsigned nib(uint32_t w, unsigned i) { return (w >> ((7 - i) * 4)) & 15; }
bool width(unsigned w) { return w == 1 || w == 2 || w == 4 || w == 8; }
uint64_t mask(unsigned w) { return w == 8 ? UINT64_MAX : (uint64_t(1) << (8 * w)) - 1; }
uint64_t immediate(const Instruction& i, unsigned start, unsigned w) {
    return w == 8 ? (uint64_t(i.words[start]) << 32) | i.words[start + 1] : uint64_t(i.words[start]) & mask(w);
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
bool decode(Instruction& i, std::string& reason) {
    const uint32_t w = i.words[0];
    const unsigned top = nib(w, 0), t = nib(w, 1);
    i.type = uint16_t(top < 0xc ? top : w >> 24);
    if ((w >> 24) == 0xff) i.type = uint16_t(w >> 20);
    auto bad = [&](const char* message) { reason = message; return false; };
    i.count = 1;
    switch (i.type) {
    case 0: case 1:
        if (!width(t) || nib(w, 2) > 4) return bad("invalid memory width or region");
        if (i.type == 0 && (w & 0x0000ff00u)) return bad("type 0 reserved bits must be zero");
        if (i.type == 1 && (nib(w, 3) < 1 || nib(w, 3) > 6 || nib(w, 4) > 1)) return bad("invalid memory condition or operand type");
        i.count = uint8_t(t == 8 ? 4 : 3); break;
    case 2:
        if (t > 1 || (w & 0x00ffffffu)) return bad("invalid end/else instruction"); break;
    case 3:
        if (t > 1 || nib(w, 2) || (w & 0xffffu)) return bad("invalid loop instruction");
        i.count = uint8_t(t ? 1 : 2); break;
    case 4:
        if (t || nib(w, 2) || (w & 0xffffu)) return bad("invalid static register load"); i.count = 3; break;
    case 5:
        if (!width(t) || nib(w, 2) > 4 || nib(w, 4) > 3 ||
            ((nib(w, 4) == 1 || nib(w, 4) == 2) && nib(w, 2)) ||
            (nib(w, 4) < 2 && nib(w, 5))) return bad("invalid memory register load");
        i.count = 2; break;
    case 6:
        if (!width(t) || nib(w, 2) || nib(w, 4) > 1 || nib(w, 5) > 1 || nib(w, 7)) return bad("invalid static-to-register-address store");
        i.count = 3; break;
    case 7:
        if (!width(t) || nib(w, 2) || nib(w, 4) > 4 || (w & 0xfffu)) return bad("invalid legacy arithmetic");
        i.count = 2; break;
    case 8: break;
    case 9:
        if (!width(t) || nib(w, 2) > 13 || nib(w, 5) > 1 || nib(w, 7) || (nib(w, 5) && nib(w, 6)) ||
            (nib(w, 2) >= 10 && t != 4 && t != 8)) return bad("invalid arithmetic width, operation, or operand");
        if (nib(w, 5)) i.count = uint8_t(t == 8 ? 3 : 2); break;
    case 0xa:
        if (!width(t) || nib(w, 4) > 1 || nib(w, 5) > 5 || (nib(w, 5) >= 3 && nib(w, 6) > 4)) return bad("invalid register memory store");
        if (nib(w, 5) == 2 || nib(w, 5) == 4 || nib(w, 5) == 5) i.count = 2; break;
    case 0xc0: {
        const unsigned t0 = nib(w, 2), mode = nib(w, 5);
        if (!width(t0) || nib(w, 3) < 1 || nib(w, 3) > 6 || mode > 5 || (mode < 2 && nib(w, 6) > 4) ||
            (mode == 4 && (w & 0xffu)) || (mode == 5 && nib(w, 7)))
            return bad("invalid register conditional");
        if (mode == 0 || mode == 2) i.count = 2;
        if (mode == 4) i.count = uint8_t(t0 == 8 ? 3 : 2);
        break;
    }
    case 0xc1:
        if (nib(w, 2) || nib(w, 4) || nib(w, 6) > 3 || nib(w, 7)) return bad("invalid saved-register instruction"); break;
    case 0xc2:
        if (nib(w, 2) > 3 || nib(w, 3)) return bad("invalid saved-register mask"); break;
    case 0xc3:
        if (w & 0x00fff000u) return bad("invalid static-register instruction"); break;
    case 0xc4:
        if (nib(w, 2) > 1 || (w & 0x000fffffu)) return bad("invalid extended keypress instruction"); i.count = 3; break;
    case 0xff0: case 0xff1: break;
    case 0xfff: {
        const unsigned mode = nib(w, 5);
        if (!width(nib(w, 3)) || mode > 4 || (mode < 2 && nib(w, 6) > 4) || (mode == 4 && nib(w, 7))) return bad("invalid debug-log instruction");
        if (mode == 0 || mode == 2) i.count = 2; break;
    }
    default: return bad("reserved or unsupported opcode");
    }
    return true;
}
bool begin_condition(uint16_t type) { return type == 1 || type == 8 || type == 0xc0 || type == 0xc4; }
bool link_blocks(Section& section, Error& error, size_t max_depth) {
    struct Block { bool loop; size_t start, alternate; unsigned reg; };
    std::vector<Block> stack;
    constexpr size_t absent = std::numeric_limits<size_t>::max();
    auto bad = [&](size_t line, const char* text) { error = {line, text}; return false; };
    for (size_t pc = 0; pc < section.code.size(); ++pc) {
        auto& i = section.code[pc]; const uint32_t w = i.words[0];
        if (begin_condition(i.type) || (i.type == 3 && nib(w, 1) == 0)) {
            if (stack.size() >= max_depth) return bad(i.line, "block nesting limit exceeded");
            stack.push_back({i.type == 3, pc, absent, nib(w, 3)});
        } else if (i.type == 2) {
            if (stack.empty() || stack.back().loop) return bad(i.line, "conditional terminator has no matching conditional");
            auto& b = stack.back();
            if (nib(w, 1)) {
                if (b.alternate != absent) return bad(i.line, "duplicate else");
                b.alternate = pc;
            } else {
                section.code[b.start].target = b.alternate == absent ? pc + 1 : b.alternate + 1;
                if (b.alternate != absent) section.code[b.alternate].target = pc + 1;
                stack.pop_back();
            }
        } else if (i.type == 3) {
            if (stack.empty() || !stack.back().loop || stack.back().reg != nib(w, 3)) return bad(i.line, "loop end does not match its start register");
            section.code[stack.back().start].target = pc + 1;
            i.target = stack.back().start + 1;
            stack.pop_back();
        }
    }
    if (!stack.empty()) return bad(section.code[stack.back().start].line, "unterminated loop or conditional");
    return true;
}
bool compare(uint64_t left, uint64_t right, unsigned condition) {
    switch (condition) { case 1: return left > right; case 2: return left >= right; case 3: return left < right;
        case 4: return left <= right; case 5: return left == right; case 6: return left != right; default: return false; }
}
uint64_t arithmetic(uint64_t a, uint64_t b, unsigned op, unsigned w) {
    a &= mask(w); b &= mask(w);
    if (op >= 10) {
        if (w == 4) {
            const uint32_t aa = uint32_t(a), bb = uint32_t(b); float x, y, z;
            std::memcpy(&x, &aa, 4); std::memcpy(&y, &bb, 4);
            z = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
            uint32_t result; std::memcpy(&result, &z, 4); return result;
        }
        double x, y, z; std::memcpy(&x, &a, 8); std::memcpy(&y, &b, 8);
        z = op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y;
        uint64_t result; std::memcpy(&result, &z, 8); return result;
    }
    uint64_t result = 0;
    switch (op) {
    case 0: result = a + b; break; case 1: result = a - b; break; case 2: result = a * b; break;
    case 3: result = b >= 8 * w ? 0 : a << b; break; case 4: result = b >= 8 * w ? 0 : a >> b; break;
    case 5: result = a & b; break; case 6: result = a | b; break; case 7: result = ~a; break;
    case 8: result = a ^ b; break; case 9: result = a; break;
    }
    return result & mask(w);
}
}  // namespace

bool parse(std::string_view text, Program& output, Error& error, ParseLimits limits) {
    error = {};
    if (text.size() > limits.max_bytes) { error = {1, "input size limit exceeded"}; return false; }
    if (text.size() >= 3 && text.substr(0, 3) == std::string_view("\xef\xbb\xbf", 3)) text.remove_prefix(3);
    Program result;
    std::vector<std::pair<uint32_t, size_t>> words;
    size_t line = 0, total_words = 0;
    auto finish = [&]() {
        if (result.sections.empty()) return true;
        auto& section = result.sections.back();
        for (size_t cursor = 0; cursor < words.size();) {
            Instruction i; i.words[0] = words[cursor].first; i.line = words[cursor].second;
            std::string reason;
            if (!decode(i, reason)) { error = {i.line, std::move(reason)}; return false; }
            if (words.size() - cursor < i.count) { error = {i.line, "truncated instruction"}; return false; }
            for (unsigned k = 1; k < i.count; ++k) i.words[k] = words[cursor + k].first;
            section.code.push_back(i); cursor += i.count;
        }
        words.clear();
        return link_blocks(section, error, limits.max_depth);
    };
    while (!text.empty()) {
        ++line;
        const size_t end = text.find('\n');
        auto row = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!row.empty() && row.front() != '[' && row.front() != '{') {
            const size_t comment = std::min(row.find('#'), row.find("//"));
            row = trim(row.substr(0, comment));
        }
        if (row.empty()) continue;
        if (row.front() == '[' || row.front() == '{') {
            const char close = row.front() == '[' ? ']' : '}';
            if (row.size() < 3 || row.back() != close || trim(row.substr(1, row.size() - 2)).empty()) {
                error = {line, "invalid section header"}; return false;
            }
            if (row.size() > 258 || result.sections.size() >= limits.max_sections) { error = {line, "section limit exceeded"}; return false; }
            if (!finish()) return false;
            Section section; section.name = std::string(trim(row.substr(1, row.size() - 2))); section.master = row.front() == '{';
            result.sections.push_back(std::move(section));
            continue;
        }
        if (result.sections.empty()) { error = {line, "opcode before any section header"}; return false; }
        while (!row.empty()) {
            const size_t end_word = row.find_first_of(" \t\r");
            const auto token = row.substr(0, end_word);
            if (token.size() != 8) { error = {line, "opcode words must have exactly eight hexadecimal digits"}; return false; }
            uint32_t value = 0;
            for (char c : token) {
                unsigned digit;
                if (c >= '0' && c <= '9') digit = unsigned(c - '0');
                else if (c >= 'a' && c <= 'f') digit = unsigned(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A' + 10);
                else { error = {line, "non-hexadecimal opcode word"}; return false; }
                value = (value << 4) | digit;
            }
            if (++total_words > limits.max_words) { error = {line, "opcode word limit exceeded"}; return false; }
            words.push_back({value, line});
            row = end_word == std::string_view::npos ? std::string_view{} : trim(row.substr(end_word));
        }
    }
    if (!finish()) return false;
    output = std::move(result); return true;
}

Vm::Vm(Program program) : program_(std::move(program)) {
    for (auto& section : program_.sections) {
        for (auto& instruction : section.code) {
            const unsigned count = instruction.count;
            std::string reason;
            if (!decode(instruction, reason) || instruction.count != count)
                throw std::invalid_argument("invalid decoded instruction");
        }
        Error error;
        if (!link_blocks(section, error, 64)) throw std::invalid_argument(error.message);
        latches_.emplace_back(section.code.size(), uint8_t{0});
    }
}
bool Vm::enable(size_t section, bool enabled) {
    if (section >= program_.sections.size()) return false;
    program_.sections[section].enabled = enabled;
    std::fill(latches_[section].begin(), latches_[section].end(), uint8_t{0});
    return true;
}
void Vm::reset() {
    registers_.fill(0); saved_.fill(0); static_.fill(0);
    for (auto& latch : latches_) std::fill(latch.begin(), latch.end(), uint8_t{0});
}

Result Vm::run(Memory& memory, size_t budget) {
    Result result;
    registers_.fill(0); saved_.fill(0);
    bool paused_here = false;
    auto fail = [&](size_t line, const char* reason) {
        result.ok = false; result.error = {line, reason};
        if (paused_here && !memory.resume()) result.error.message += "; automatic resume also failed";
        return result;
    };
    auto base = [&](unsigned region) -> uint64_t {
        switch (region) { case 0: return memory.main_nso_base(); case 1: return memory.heap_base();
            case 2: return memory.alias_base(); case 3: return memory.aslr_base(); default: return 0; }
    };
    auto add = [](uint64_t a, uint64_t b, uint64_t& out) { if (b > UINT64_MAX - a) return false; out = a + b; return true; };
    auto read = [&](uint64_t address, unsigned w, uint64_t& out) {
        if (address > UINT64_MAX - (w - 1)) return false;
        uint8_t bytes[8]{}; if (!memory.read(address, bytes, w)) return false;
        out = 0; for (unsigned j = 0; j < w; ++j) out |= uint64_t(bytes[j]) << (8 * j); return true;
    };
    auto write = [&](uint64_t address, unsigned w, uint64_t value) {
        if (address > UINT64_MAX - (w - 1)) return false;
        uint8_t bytes[8]{}; for (unsigned j = 0; j < w; ++j) bytes[j] = uint8_t(value >> (8 * j));
        return memory.write(address, bytes, w);
    };
    for (unsigned master_pass = 0; master_pass < 2; ++master_pass) for (size_t s = 0; s < program_.sections.size(); ++s) {
        auto& section = program_.sections[s]; result.section = s;
        if (section.master != (master_pass == 0) || (!section.master && !section.enabled)) continue;
        for (size_t pc = 0; pc < section.code.size();) {
            const Instruction& i = section.code[pc]; const uint32_t w = i.words[0];
            if (result.steps >= budget) return fail(i.line, "step budget exhausted");
            ++result.steps;
            unsigned n[8]; for (unsigned k = 0; k < 8; ++k) n[k] = nib(w, k);
            const unsigned t = i.type >= 0xc0 ? (i.type == 0xfff ? n[3] : n[2]) : n[1];
            uint64_t address = 0, value = 0;
            const uint64_t offset40 = (uint64_t(w & 0xffu) << 32) | i.words[1];
            auto region_address = [&](unsigned region, uint64_t offset, uint64_t reg) {
                return add(base(region), offset, address) && add(address, reg, address);
            };
            auto operand = [&](unsigned mode, unsigned x, unsigned y, unsigned bytes, bool log) {
                const uint64_t offset36 = (uint64_t(y) << 32) | i.words[1];
                switch (mode) {
                case 0: if (!region_address(x, offset36, 0)) return false; return read(address, bytes, value);
                case 1: if (!region_address(x, 0, registers_[y])) return false; return read(address, bytes, value);
                case 2: if (!add(registers_[x], offset36, address)) return false; return read(address, bytes, value);
                case 3: if (!add(registers_[x], registers_[y], address)) return false; return read(address, bytes, value);
                case 4: value = log ? registers_[x] & mask(bytes) : immediate(i, 1, bytes); return true;
                case 5: value = registers_[x] & mask(bytes); return true;
                default: return false;
                }
            };
            bool condition = true;
            switch (i.type) {
            case 0:
                if (!region_address(n[2], offset40, registers_[n[3]]) || !write(address, t, immediate(i, 2, t))) return fail(i.line, "memory write failed or address overflow");
                break;
            case 1:
                if (!region_address(n[2], offset40, n[4] ? registers_[n[5]] : 0) || !read(address, t, value)) return fail(i.line, "conditional memory read failed or address overflow");
                condition = compare(value, immediate(i, 2, t), n[3]); break;
            case 2: if (n[1]) { pc = i.target; continue; } break;
            case 3:
                if (!n[1]) { registers_[n[3]] = i.words[1]; if (!registers_[n[3]]) { pc = i.target; continue; } }
                else if (registers_[n[3]] > 1) { --registers_[n[3]]; pc = i.target; continue; }
                else registers_[n[3]] = 0;
                break;
            case 4: registers_[n[3]] = immediate(i, 1, 8); break;
            case 5:
                if (n[4] == 0 || n[4] == 3) {
                    if (!region_address(n[2], offset40, n[4] == 3 ? registers_[n[5]] : 0)) return fail(i.line, "load address overflow");
                } else if (!add(registers_[n[4] == 1 ? n[3] : n[5]], offset40, address)) return fail(i.line, "load address overflow");
                if (!read(address, t, value)) return fail(i.line, "memory register load failed");
                registers_[n[3]] = value; break;
            case 6:
                if (!add(registers_[n[3]], n[5] ? registers_[n[6]] : 0, address) || !write(address, t, immediate(i, 1, 8))) return fail(i.line, "static register-address store failed");
                if (n[4] && !add(registers_[n[3]], t, registers_[n[3]])) return fail(i.line, "register increment overflow");
                break;
            case 7: registers_[n[3]] = arithmetic(registers_[n[3]], i.words[1], n[4], t); break;
            case 8: value = w & 0x0fffffffu; condition = (memory.keys_held() & value) == value; break;
            case 9: registers_[n[3]] = arithmetic(registers_[n[4]], n[5] ? immediate(i, 1, t) : registers_[n[6]], n[2], t); break;
            case 0xa: {
                const unsigned mode = n[5]; const uint64_t fixed = (uint64_t(n[7]) << 32) | i.words[1];
                bool ok;
                if (mode >= 3) ok = region_address(n[6], mode >= 4 ? fixed : 0, mode == 4 ? 0 : registers_[n[3]]);
                else ok = add(registers_[n[3]], mode == 1 ? registers_[n[6]] : mode == 2 ? fixed : 0, address);
                if (!ok || !write(address, t, registers_[n[2]])) return fail(i.line, "register memory store failed or address overflow");
                if (n[4] && !add(registers_[n[3]], t, registers_[n[3]])) return fail(i.line, "register increment overflow");
                break;
            }
            case 0xc0:
                if (!operand(n[5], n[6], n[7], t, false)) return fail(i.line, "register conditional operand failed");
                condition = compare(registers_[n[4]] & mask(t), value, n[3]); break;
            case 0xc1:
                if (n[6] == 0) registers_[n[3]] = saved_[n[5]];
                if (n[6] == 1) saved_[n[3]] = registers_[n[5]];
                if (n[6] == 2) saved_[n[3]] = 0;
                if (n[6] == 3) registers_[n[3]] = 0;
                break;
            case 0xc2:
                for (unsigned r = 0; r < 16; ++r) if (w & (1u << r)) {
                    if (n[2] == 0) registers_[r] = saved_[r]; if (n[2] == 1) saved_[r] = registers_[r];
                    if (n[2] == 2) saved_[r] = 0; if (n[2] == 3) registers_[r] = 0;
                }
                break;
            case 0xc3: {
                const unsigned index = (w >> 4) & 0xff;
                if (index < 128) registers_[n[7]] = static_[index]; else static_[index - 128] = registers_[n[7]];
                break;
            }
            case 0xc4: {
                const uint64_t keys = immediate(i, 1, 8); const bool held = (memory.keys_held() & keys) == keys;
                condition = held && (n[2] || !latches_[s][pc]); latches_[s][pc] = uint8_t(held); break;
            }
            case 0xff0: if (!memory.pause()) return fail(i.line, "pause callback failed"); paused_here = true; break;
            case 0xff1: if (!memory.resume()) return fail(i.line, "resume callback failed"); paused_here = false; break;
            case 0xfff:
                if (!operand(n[5], n[6], n[7], t, true)) return fail(i.line, "debug-log operand read failed");
                memory.debug_log(n[4], value, t); break;
            default: return fail(i.line, "unvalidated opcode in program");
            }
            if (begin_condition(i.type) && !condition) pc = i.target;
            else ++pc;
        }
    }
    return result;
}
}  // namespace cheat
