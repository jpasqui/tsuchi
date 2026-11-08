#include "cheat_vm.h"

#include <string.h>

namespace tsuchi::cheat {
namespace {

// ---- Reading the text -----------------------------------------------------

void say(char* problem, size_t capacity, const char* text) {
    if (!problem || !capacity) return;
    size_t length = strlen(text);
    if (length >= capacity) length = capacity - 1;
    memcpy(problem, text, length);
    problem[length] = 0;
}

// UTF-8 with no control characters but tab and line ends, and no byte order mark.
bool clean_text(const uint8_t* text, uint64_t size) {
    for (uint64_t i = 0; i < size;) {
        const uint8_t c = text[i];
        if (c < 0x80) {
            if ((c < 0x20 && c != '\t' && c != '\r' && c != '\n') || c == 0x7F) return false;
            ++i;
            continue;
        }
        unsigned more;
        uint32_t code;
        if (c >= 0xC2 && c <= 0xDF) more = 1, code = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF) more = 2, code = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4) more = 3, code = c & 0x07;
        else return false;
        if (size - i <= more) return false;
        for (unsigned k = 1; k <= more; ++k) {
            if ((text[i + k] & 0xC0) != 0x80) return false;
            code = (code << 6) | (text[i + k] & 0x3F);
        }
        if ((more == 2 && code < 0x800) || (more == 3 && (code < 0x10000 || code > 0x10FFFF)) ||
            (code >= 0xD800 && code <= 0xDFFF)) return false;
        // C1 controls, and the byte order mark anywhere.
        if ((code >= 0x80 && code <= 0x9F) || code == 0xFEFF) return false;
        i += more + 1;
    }
    return true;
}

bool hex_word(const char* at, uint32_t& out) {
    uint32_t value = 0;
    for (int i = 0; i < 8; ++i) {
        const char c = at[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f') digit = uint32_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = uint32_t(c - 'A' + 10);
        else return false;
        value = value << 4 | digit;
    }
    out = value;
    return true;
}

bool name_ok(const char* name, size_t length) {
    if (!length || length >= kNameBytes) return false;
    bool visible = false;
    for (size_t i = 0; i < length; ++i) {
        const char c = name[i];
        if (c == '[' || c == ']' || c == '{' || c == '}' || c == '\t') return false;
        if (c != ' ') visible = true;
    }
    return visible;
}

bool same_name(const File& file, const char* name) {
    if (file.has_master && !strcmp(file.master.name, name)) return true;
    for (uint32_t i = 0; i < file.count; ++i)
        if (!strcmp(file.cheats[i].name, name)) return true;
    return false;
}

// ---- Instructions ---------------------------------------------------------

uint32_t nibble(uint32_t word, unsigned index) { return (word >> (28 - 4 * index)) & 0xF; }

bool width_ok(uint32_t width) { return width == 1 || width == 2 || width == 4 || width == 8; }
uint64_t width_mask(uint32_t width) { return width == 8 ? ~uint64_t(0) : (uint64_t(1) << (8 * width)) - 1; }

// How many words the instruction at `ip` takes, and false when it can't be read: an unknown type, a bad width,
// region or operation, or words missing at the end.
bool length_of(const uint32_t* words, uint32_t count, uint32_t ip, uint32_t& length) {
    const uint32_t first = words[ip];
    const uint32_t type = nibble(first, 0);
    length = 0;
    switch (type) {
    case 0x0:
    case 0x1: {
        const uint32_t width = nibble(first, 1), region = nibble(first, 2);
        if (!width_ok(width) || region > 3) return false;
        if (type == 0x1 && (nibble(first, 3) < 1 || nibble(first, 3) > 6 || nibble(first, 4) || nibble(first, 5))) return false;
        if (type == 0x0 && (nibble(first, 4) || nibble(first, 5))) return false;
        length = width == 8 ? 4 : 3;
        break;
    }
    case 0x2: if (nibble(first, 1) > 1) return false; length = 1; break;
    case 0x3: if (nibble(first, 1) > 1) return false; length = nibble(first, 1) ? 1 : 2; break;
    case 0x4: length = 3; break;
    case 0x5:
        if (!width_ok(nibble(first, 1)) || nibble(first, 2) > 3 || nibble(first, 4) > 1) return false;
        length = 2;
        break;
    case 0x6: if (!width_ok(nibble(first, 1))) return false; length = 3; break;
    case 0x7: if (!width_ok(nibble(first, 1)) || nibble(first, 4) > 4) return false; length = 2; break;
    case 0x8: length = 1; break;
    case 0x9: {
        const uint32_t width = nibble(first, 1);
        if (!width_ok(width) || nibble(first, 2) > 9 || nibble(first, 5) > 1) return false;
        length = nibble(first, 5) ? (width == 8 ? 3 : 2) : 1;
        break;
    }
    case 0xA: {
        const uint32_t kind = nibble(first, 5);
        if (!width_ok(nibble(first, 1)) || kind > 5 || ((kind >= 3) && nibble(first, 6) > 3)) return false;
        length = kind == 2 || kind == 4 || kind == 5 ? 2 : 1;
        break;
    }
    case 0xC:
        switch (nibble(first, 1)) {
        case 0x0: {
            const uint32_t width = nibble(first, 2), operand = nibble(first, 5);
            if (!width_ok(width) || nibble(first, 3) < 1 || nibble(first, 3) > 6 || operand > 5) return false;
            if ((operand == 0 || operand == 1) && nibble(first, 6) > 3) return false;
            length = operand == 0 || operand == 2 ? 2 : operand == 4 ? (width == 8 ? 3 : 2) : 1;
            break;
        }
        case 0x1: if (nibble(first, 6) > 3) return false; length = 1; break;
        case 0x2: if (nibble(first, 2) > 3) return false; length = 1; break;
        case 0x3: length = 1; break;
        case 0x4: length = 3; break;
        default: return false;
        }
        break;
    case 0xF:
        if (nibble(first, 1) != 0xF) return false;
        switch (nibble(first, 2)) {
        case 0x0:
        case 0x1: length = 1; break;
        case 0xF: {
            const uint32_t operand = nibble(first, 5);
            if (!width_ok(nibble(first, 3)) || operand > 4) return false;
            length = operand == 0 || operand == 2 ? 2 : 1;
            break;
        }
        default: return false;
        }
        break;
    default: return false;
    }
    return length && length <= count - ip;
}

bool is_condition(uint32_t first) {
    const uint32_t type = nibble(first, 0);
    return type == 0x1 || type == 0x8 || (type == 0xC && (nibble(first, 1) == 0x0 || nibble(first, 1) == 0x4));
}

bool compare(uint32_t condition, uint64_t a, uint64_t b) {
    switch (condition) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return false;
    }
}

struct Machine {
    const uint32_t* words;
    const uint16_t* owners;
    uint32_t count;
    const Memory& memory;
    State& state;
    Report report{};
    uint64_t registers[kRegisters]{};
    uint64_t saved[kRegisters]{};
    uint32_t loop_tops[kRegisters]{};
    uint32_t ip = 0;
    uint32_t depth = 0; // conditions entered and not yet ended

    Machine(const uint32_t* w, const uint16_t* o, uint32_t c, const Memory& m, State& s)
        : words(w), owners(o), count(c), memory(m), state(s) {}

    uint16_t owner(uint32_t at) const { return owners ? owners[at] : kMaster; }

    uint64_t region(uint32_t index) const { return memory.base[index & 3]; }

    uint64_t load(uint64_t address, uint32_t width) {
        uint64_t value = 0;
        if (!memory.read || !memory.read(memory.user, address, &value, width)) {
            ++report.faults;
            return 0;
        }
        return value & width_mask(width);
    }

    void store(uint64_t address, uint64_t value, uint32_t width, uint32_t at) {
        value &= width_mask(width);
        if (!memory.write || !memory.write(memory.user, address, &value, width, owner(at))) ++report.faults;
    }

    // Skips from a condition that didn't hold. Stops after its End, or at an Else of the same depth when
    // `stop_at_else`, so that the Else's part runs.
    bool skip(bool stop_at_else) {
        uint32_t nested = 0;
        while (ip < count) {
            uint32_t length;
            if (!length_of(words, count, ip, length)) return false;
            const uint32_t first = words[ip];
            ip += length;
            if (is_condition(first)) {
                ++nested;
            } else if (nibble(first, 0) == 0x2) {
                const bool otherwise = nibble(first, 1) == 1;
                if (!nested) {
                    if (otherwise && stop_at_else) {
                        ++depth; // the Else's part runs, and its End closes it
                        return true;
                    }
                    if (!otherwise) return true;
                } else if (!otherwise) {
                    --nested;
                }
            }
        }
        return true;
    }

    void condition(bool holds) {
        if (holds) ++depth;
        else if (!skip(true)) stop();
    }

    void stop() {
        report.stopped = true;
        report.stopped_at = ip;
        report.stopped_owner = ip < count ? owner(ip) : kMaster;
        ip = count;
    }

    // The second operand's address for C0 and FFF, by its operand kind.
    uint64_t operand_address(uint32_t first, uint32_t second, uint32_t kind) {
        switch (kind) {
        case 0: return region(nibble(first, 6)) + ((uint64_t(first & 0xF) << 32) | second);
        case 1: return region(nibble(first, 6)) + registers[nibble(first, 7)];
        case 2: return registers[nibble(first, 6)] + ((uint64_t(first & 0xF) << 32) | second);
        default: return registers[nibble(first, 6)] + registers[nibble(first, 7)];
        }
    }

    void step() {
        uint32_t length;
        const uint32_t at = ip;
        if (!length_of(words, count, ip, length)) return stop();
        const uint32_t* w = words + ip;
        const uint32_t first = w[0];
        ip += length;
        ++report.steps;
        switch (nibble(first, 0)) {
        case 0x0: { // store a value at region + address + register
            const uint32_t width = nibble(first, 1);
            const uint64_t address = region(nibble(first, 2)) + ((uint64_t(first & 0xFF) << 32) | w[1]) + registers[nibble(first, 3)];
            const uint64_t value = width == 8 ? (uint64_t(w[2]) << 32 | w[3]) : w[2];
            store(address, value, width, at);
            break;
        }
        case 0x1: { // if the memory at region + address compares to a value
            const uint32_t width = nibble(first, 1);
            const uint64_t address = region(nibble(first, 2)) + ((uint64_t(first & 0xFF) << 32) | w[1]);
            const uint64_t value = (width == 8 ? (uint64_t(w[2]) << 32 | w[3]) : w[2]) & width_mask(width);
            condition(compare(nibble(first, 3), load(address, width), value));
            break;
        }
        case 0x2: // End, or Else reached from the part that ran
            if (nibble(first, 1) == 1) {
                if (depth) --depth;
                if (!skip(false)) stop();
            } else if (depth) {
                --depth;
            }
            break;
        case 0x3: { // loop
            const uint32_t reg = nibble(first, 3);
            if (!nibble(first, 1)) {
                registers[reg] = w[1];
                loop_tops[reg] = ip;
            } else if (--registers[reg]) {
                ip = loop_tops[reg];
            }
            break;
        }
        case 0x4: registers[nibble(first, 3)] = uint64_t(w[1]) << 32 | w[2]; break;
        case 0x5: { // load a register from memory
            const uint32_t width = nibble(first, 1), reg = nibble(first, 3);
            const uint64_t offset = (uint64_t(first & 0xFF) << 32) | w[1];
            const uint64_t address = nibble(first, 4) ? registers[reg] + offset : region(nibble(first, 2)) + offset;
            registers[reg] = load(address, width);
            break;
        }
        case 0x6: { // store a value at a register's address
            const uint32_t width = nibble(first, 1), reg = nibble(first, 3);
            uint64_t address = registers[reg];
            if (nibble(first, 5)) address += registers[nibble(first, 6)];
            store(address, uint64_t(w[1]) << 32 | w[2], width, at);
            if (nibble(first, 4)) registers[reg] += width;
            break;
        }
        case 0x7: { // arithmetic with a value, the older form
            const uint32_t width = nibble(first, 1), reg = nibble(first, 3);
            const uint64_t value = w[1];
            uint64_t& target = registers[reg];
            switch (nibble(first, 4)) {
            case 0: target += value; break;
            case 1: target -= value; break;
            case 2: target *= value; break;
            case 3: target = value >= 64 ? 0 : target << value; break;
            case 4: target = value >= 64 ? 0 : target >> value; break;
            }
            target &= width_mask(width);
            break;
        }
        case 0x8: { // if these buttons are held
            const uint64_t mask = first & 0x0FFFFFFF;
            condition((memory.keys & mask) == mask);
            break;
        }
        case 0x9: { // arithmetic between registers, or with a value
            const uint32_t width = nibble(first, 1), operation = nibble(first, 2);
            const uint64_t a = registers[nibble(first, 4)];
            const uint64_t b = nibble(first, 5) ? (width == 8 ? uint64_t(w[1]) << 32 | w[2] : w[1]) : registers[nibble(first, 6)];
            uint64_t result = 0;
            switch (operation) {
            case 0: result = a + b; break;
            case 1: result = a - b; break;
            case 2: result = a * b; break;
            case 3: result = b >= 64 ? 0 : a << b; break;
            case 4: result = b >= 64 ? 0 : a >> b; break;
            case 5: result = a & b; break;
            case 6: result = a | b; break;
            case 7: result = ~a; break;
            case 8: result = a ^ b; break;
            case 9: result = a; break;
            }
            registers[nibble(first, 3)] = result & width_mask(width);
            break;
        }
        case 0xA: { // store a register at an address
            const uint32_t width = nibble(first, 1), source = nibble(first, 2), reg = nibble(first, 3);
            const uint32_t kind = nibble(first, 5);
            const uint64_t immediate = length > 1 ? (uint64_t(first & 0xF) << 32 | w[1]) : 0;
            uint64_t address = 0;
            switch (kind) {
            case 0: address = registers[reg]; break;
            case 1: address = registers[reg] + registers[nibble(first, 6)]; break;
            case 2: address = registers[reg] + immediate; break;
            case 3: address = region(nibble(first, 6)) + registers[reg]; break;
            case 4: address = region(nibble(first, 6)) + immediate; break;
            case 5: address = region(nibble(first, 6)) + registers[reg] + immediate; break;
            }
            store(address, registers[source], width, at);
            if (nibble(first, 4)) registers[reg] += width;
            break;
        }
        case 0xC:
            switch (nibble(first, 1)) {
            case 0x0: { // if a register compares to memory, a value, or a register
                const uint32_t width = nibble(first, 2), kind = nibble(first, 5);
                const uint64_t left = registers[nibble(first, 4)] & width_mask(width);
                uint64_t right;
                if (kind == 4) right = (width == 8 ? uint64_t(w[1]) << 32 | w[2] : w[1]) & width_mask(width);
                else if (kind == 5) right = registers[nibble(first, 6)] & width_mask(width);
                else right = load(operand_address(first, length > 1 ? w[1] : 0, kind), width);
                condition(compare(nibble(first, 3), left, right));
                break;
            }
            case 0x1: { // save, restore or clear one register
                const uint32_t target = nibble(first, 3), source = nibble(first, 5);
                switch (nibble(first, 6)) {
                case 0: registers[target] = saved[source]; break;
                case 1: saved[target] = registers[source]; break;
                case 2: saved[target] = 0; break;
                case 3: registers[target] = 0; break;
                }
                break;
            }
            case 0x2: { // the same for the registers a mask names
                const uint32_t operation = nibble(first, 2), mask = first & 0xFFFF;
                for (uint32_t i = 0; i < kRegisters; ++i) {
                    if (!(mask & (1u << i))) continue;
                    switch (operation) {
                    case 0: registers[i] = saved[i]; break;
                    case 1: saved[i] = registers[i]; break;
                    case 2: saved[i] = 0; break;
                    case 3: registers[i] = 0; break;
                    }
                }
                break;
            }
            case 0x3: { // read or write a static register
                const uint32_t index = (first >> 4) & 0xFF, reg = first & 0xF;
                if (index < 0x80) registers[reg] = state.statics[index];
                else state.statics[index] = registers[reg];
                break;
            }
            case 0x4: { // if these buttons are held, or were just pressed
                const uint64_t mask = uint64_t(w[1]) << 32 | w[2];
                const uint64_t keys = nibble(first, 2) ? memory.keys : memory.keys & ~memory.previous_keys;
                condition((keys & mask) == mask && mask);
                break;
            }
            }
            break;
        case 0xF: // pause, resume, debug log: nothing to do here
            break;
        }
    }

    void execute() {
        while (ip < count) {
            if (report.steps >= kStepLimit) {
                report.step_limit = true;
                report.stopped_at = ip;
                report.stopped_owner = owner(ip);
                return;
            }
            step();
        }
    }
};

} // namespace

TSUCHI_Result parse(const char* text, uint64_t size, File& out, char* problem, size_t capacity) {
    if (!text || size >= kFileBytes) return say(problem, capacity, "a cheat file must be smaller than 64 KiB"), TSUCHI_INVALID;
    if (!clean_text(reinterpret_cast<const uint8_t*>(text), size))
        return say(problem, capacity, "a cheat file must be UTF-8 without control characters or a byte order mark"), TSUCHI_INVALID;
    File& file = out;
    memset(&file, 0, sizeof file);
    Block* current = nullptr;
    uint64_t at = 0;
    while (at < size) {
        uint64_t end = at;
        while (end < size && text[end] != '\n' && text[end] != '\r') ++end;
        uint64_t first = at, last = end;
        while (first < last && (text[first] == ' ' || text[first] == '\t')) ++first;
        while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t')) --last;
        at = end + 1;
        if (first == last) continue;
        const char open = text[first];
        if (open == '[' || open == '{') {
            const char close = open == '[' ? ']' : '}';
            if (last - first < 2 || text[last - 1] != close)
                return say(problem, capacity, "a cheat header is malformed or shares its line with words"), TSUCHI_INVALID;
            const char* name = text + first + 1;
            const size_t length = size_t(last - first - 2);
            if (!name_ok(name, length))
                return say(problem, capacity, "a cheat name must be 1 to 63 bytes without brackets, braces or tabs"), TSUCHI_INVALID;
            char copy[kNameBytes];
            memcpy(copy, name, length);
            copy[length] = 0;
            if (same_name(file, copy)) return say(problem, capacity, "cheat names must be unique"), TSUCHI_INVALID;
            if (open == '{') {
                if (file.has_master) return say(problem, capacity, "a cheat file may have only one master block"), TSUCHI_INVALID;
                file.has_master = true;
                current = &file.master;
            } else {
                if (file.count >= kCheats) return say(problem, capacity, "a cheat file may have at most 127 named blocks"), TSUCHI_INVALID;
                current = &file.cheats[file.count++];
            }
            memcpy(current->name, copy, length + 1);
            continue;
        }
        if (!current) return say(problem, capacity, "cheat words need a header before them"), TSUCHI_INVALID;
        for (uint64_t i = first; i < last;) {
            uint64_t word_end = i;
            while (word_end < last && text[word_end] != ' ' && text[word_end] != '\t') ++word_end;
            uint32_t word;
            if (word_end - i != 8 || !hex_word(text + i, word))
                return say(problem, capacity, "a cheat word must be eight hexadecimal digits"), TSUCHI_INVALID;
            if (current->count >= kWords) return say(problem, capacity, "a cheat block may have at most 256 words"), TSUCHI_INVALID;
            current->words[current->count++] = word;
            i = word_end;
            while (i < last && (text[i] == ' ' || text[i] == '\t')) ++i;
        }
    }
    if (file.has_master && !file.master.count) return say(problem, capacity, "a cheat block must have words"), TSUCHI_INVALID;
    for (uint32_t i = 0; i < file.count; ++i)
        if (!file.cheats[i].count) return say(problem, capacity, "a cheat block must have words"), TSUCHI_INVALID;
    if (!file.count) return say(problem, capacity, "a cheat file needs at least one named block"), TSUCHI_INVALID;
    return TSUCHI_OK;
}

Report run(const uint32_t* words, const uint16_t* owners, uint32_t count, const Memory& memory, State& state) {
    Machine machine(words, owners, count, memory, state);
    if (words && count) machine.execute();
    return machine.report;
}

bool decodable(const uint32_t* words, uint32_t count, uint32_t& stopped_at) {
    for (uint32_t ip = 0; ip < count;) {
        uint32_t length;
        if (!length_of(words, count, ip, length)) {
            stopped_at = ip;
            return false;
        }
        ip += length;
    }
    return true;
}

} // namespace tsuchi::cheat
