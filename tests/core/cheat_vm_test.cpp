// Checks of the cheat interpreter and of the runtime that turns cheats on and off while the game runs, against
// memory and a native patch backend of the test's own. Exits with the number of failures.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_game.h"
#include "callable_thunks.h"
#include "cheat_runtime.h"
#include "cheat_vm.h"
#include "hooks.h"
#include "registry.h"

namespace tsuchi {
// The frame tick isn't part of this program; the checks call cheats_frame themselves.
bool each_engine_frame(void (*)()) { return true; }
} // namespace tsuchi

namespace {

namespace cheat = tsuchi::cheat;

int failures = 0;
#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

// ---- Memory for the interpreter ----

alignas(8) uint8_t main_memory[4096];
alignas(8) uint8_t heap_memory[4096];
uint64_t main_at() { return reinterpret_cast<uint64_t>(main_memory); }
uint64_t heap_at() { return reinterpret_cast<uint64_t>(heap_memory); }
bool inside(uint64_t address, uint32_t bytes) {
    return (address >= main_at() && address + bytes <= main_at() + sizeof main_memory) ||
           (address >= heap_at() && address + bytes <= heap_at() + sizeof heap_memory);
}
std::vector<uint16_t> writers;
bool vm_read(void*, uint64_t address, void* out, uint32_t bytes) {
    if (!inside(address, bytes)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
    return true;
}
bool vm_write(void*, uint64_t address, const void* value, uint32_t bytes, uint16_t owner) {
    if (!inside(address, bytes)) return false;
    std::memcpy(reinterpret_cast<void*>(address), value, bytes);
    writers.push_back(owner);
    return true;
}
template <class T>
T at(uint8_t* memory, uint64_t offset) {
    T value;
    std::memcpy(&value, memory + offset, sizeof value);
    return value;
}
template <class T>
void put(uint8_t* memory, uint64_t offset, T value) {
    std::memcpy(memory + offset, &value, sizeof value);
}

cheat::Report run(const std::vector<uint32_t>& words, uint64_t keys = 0, uint64_t previous = 0, cheat::State* state = nullptr) {
    static cheat::State fresh;
    if (!state) {
        fresh = {};
        state = &fresh;
    }
    cheat::Memory memory{{main_at(), heap_at(), 0, 0}, nullptr, vm_read, vm_write, keys, previous};
    return cheat::run(words.data(), nullptr, uint32_t(words.size()), memory, *state);
}
void clear() {
    std::memset(main_memory, 0, sizeof main_memory);
    std::memset(heap_memory, 0, sizeof heap_memory);
    writers.clear();
}

cheat::File file;
char problem[128];
bool parses(const std::string& text) { return cheat::parse(text.data(), text.size(), file, problem, sizeof problem) == TSUCHI_OK; }

void parsing() {
    CHECK(parses("{Master}\n04000000 00000010 00000000 00000001\n[Infinite money]\r\n04100000 00000020 0000270F\n\n[Two words] \n 80000001\t20000000\n"));
    CHECK(file.has_master && file.master.count == 4 && !std::strcmp(file.master.name, "Master"));
    CHECK(file.count == 2 && !std::strcmp(file.cheats[0].name, "Infinite money") && file.cheats[0].count == 3);
    CHECK(file.cheats[0].words[2] == 0x270F && file.cheats[1].count == 2 && file.cheats[1].words[0] == 0x80000001);
    CHECK(parses("[Ünïcode name]\n20000000\n"));
    for (const char* bad : {"[A] 20000000\n", "20000000\n[A]\n20000000\n", "[A]\n2000000G\n", "[A]\n2000000\n",
                            "[A]\n20000000\n[A]\n20000000\n", "[A]\n[B]\n20000000\n", "{M}\n20000000\n",
                            "[]\n20000000\n", "[A\n20000000\n", "\xEF\xBB\xBF[A]\n20000000\n", "[A]\n20000000\x01\n",
                            "{M}\n20000000\n{N}\n20000000\n[A]\n20000000\n", "[A[B]]\n20000000\n", "\xC3[A]\n20000000\n"})
        CHECK(!parses(bad));
    std::string many;
    for (int i = 0; i < 128; ++i) many += "[c" + std::to_string(i) + "]\n20000000\n";
    CHECK(!parses(many));
    std::string long_block = "[A]\n";
    for (int i = 0; i < 257; ++i) long_block += "20000000\n";
    CHECK(!parses(long_block));
    std::string big = "[A]\n";
    while (big.size() < cheat::kFileBytes) big += "20000000 20000000 20000000 20000000\n";
    CHECK(!parses(big));
    CHECK(!parses(std::string("[") + std::string(64, 'n') + "]\n20000000\n"));
}

void stores_and_loads() {
    clear();
    // 0: widths 1, 2, 4 and 8 at main + offset, and heap + offset + register.
    CHECK(!run({0x01000000, 0x00000010, 0x000000AB,
                0x02000000, 0x00000012, 0x0000BEEF,
                0x04000000, 0x00000014, 0x12345678,
                0x08000000, 0x00000018, 0x01234567, 0x89ABCDEF,
                0x40010000, 0x00000000, 0x00000008, // r1 = 8
                0x04110000, 0x00000020, 0x00000005}).faults);
    CHECK(at<uint8_t>(main_memory, 0x10) == 0xAB && at<uint16_t>(main_memory, 0x12) == 0xBEEF);
    CHECK(at<uint32_t>(main_memory, 0x14) == 0x12345678 && at<uint64_t>(main_memory, 0x18) == 0x0123456789ABCDEFull);
    CHECK(at<uint32_t>(heap_memory, 0x28) == 5);
    // A value wider than its width is cut to it.
    clear();
    run({0x01000000, 0x00000000, 0x00001234});
    CHECK(at<uint16_t>(main_memory, 0) == 0x34);
    // 5: a pointer chain. Heap holds a pointer to main + 0x100; the cheat follows it and stores there.
    clear();
    put<uint64_t>(heap_memory, 0x40, main_at() + 0x100);
    put<uint64_t>(main_memory, 0x108, main_at() + 0x200);
    run({0x58100000, 0x00000040,  // r0 = [heap + 0x40]
         0x58001000, 0x00000008,  // r0 = [r0 + 8]
         0x64000000, 0x00000000, 0x00000063}); // [r0] = 0x63, 4 bytes
    CHECK(at<uint32_t>(main_memory, 0x200) == 0x63);
    // A read from memory that isn't there counts a fault and gives 0; the run goes on.
    clear();
    const cheat::Report faulted = run({0x58100000, 0x00010000, 0x04000000, 0x00000000, 0x00000007});
    CHECK(faulted.faults == 1 && at<uint32_t>(main_memory, 0) == 7);
}

void conditions_and_loops() {
    clear();
    put<uint32_t>(main_memory, 0x10, 5);
    // 1: if [main + 0x10] == 5, store 1; else store 2. Then if != 5, store 3 at 4.
    run({0x14050000, 0x00000010, 0x00000005,
         0x04000000, 0x00000000, 0x00000001,
         0x21000000,
         0x04000000, 0x00000000, 0x00000002,
         0x20000000,
         0x14060000, 0x00000010, 0x00000005,
         0x04000000, 0x00000004, 0x00000003,
         0x20000000});
    CHECK(at<uint32_t>(main_memory, 0) == 1 && at<uint32_t>(main_memory, 4) == 0);
    // A false condition skips a nested one, and its Else runs.
    clear();
    run({0x14050000, 0x00000010, 0x00000009,  // false
         0x80000001, 0x04000000, 0x00000000, 0x00000001, 0x21000000, 0x20000000,
         0x21000000,
         0x04000000, 0x00000008, 0x00000002,
         0x20000000});
    CHECK(at<uint32_t>(main_memory, 0) == 0 && at<uint32_t>(main_memory, 8) == 2);
    // 3 and 6: a loop of four stores, the address register moving on each time.
    clear();
    std::vector<uint32_t> loop = {0x40010000};
    loop.push_back(uint32_t(main_at() >> 32));
    loop.push_back(uint32_t(main_at() + 0x40));
    const std::vector<uint32_t> body = {0x30020000, 0x00000004,          // loop r2, 4 times
                                        0x64011000, 0x00000000, 0x0000002A, // [r1] = 42, r1 += 4
                                        0x31020000};
    loop.insert(loop.end(), body.begin(), body.end());
    run(loop);
    CHECK(at<uint32_t>(main_memory, 0x40) == 42 && at<uint32_t>(main_memory, 0x4C) == 42 && at<uint32_t>(main_memory, 0x50) == 0);
    // An endless loop stops at the step limit.
    const cheat::Report endless = run({0x30000000, 0xFFFFFFFF, 0x31000000, 0x30000000, 0x00000000, 0x31000000});
    CHECK(endless.step_limit && endless.steps >= cheat::kStepLimit);
}

void arithmetic_and_registers() {
    // 7: r3 = 10, +5, *3, <<1 = 90. 9: r4 = r3 - 1, r5 = r3 ^ r3, r6 = ~r3 cut to 1 byte.
    clear();
    std::vector<uint32_t> words = {0x40030000, 0x00000000, 0x0000000A,
                                   0x74030000, 0x00000005,
                                   0x74032000, 0x00000003,
                                   0x74033000, 0x00000001,
                                   0x98143100, 0x00000000, 0x00000001, // r4 = r3 - 1, width 8 value
                                   0x98853030,                          // r5 = r3 ^ r3? register form: S=3, s=3
                                   0x91763000,                          // r6 = ~r3, width 1
                                   // A, kind 4: store r3 (4 bytes) at main + 0x60, then kind 3: r6 at main + r7.
                                   0xA4300400, 0x00000060,
                                   0x40070000, 0x00000000, 0x00000070,
                                   0xA1670300,
                                   // C1: save r3 into saved 0, clear r3, restore it into r8; A kind 4 stores r8.
                                   0xC1000310, 0xC1030030, 0xC1080000,
                                   0xA4800400, 0x00000064};
    run(words);
    CHECK(at<uint32_t>(main_memory, 0x60) == 90);
    CHECK(at<uint8_t>(main_memory, 0x70) == uint8_t(~90u));
    CHECK(at<uint32_t>(main_memory, 0x64) == 90);
    // C3: a static register written in one run is read in the next.
    cheat::State state{};
    run({0x40000000, 0x00000000, 0x00000033, 0xC3000800}, 0, 0, &state); // statics[0x80] = r0
    CHECK(state.statics[0x80] == 0x33);
    state.statics[0x01] = 0x44;
    clear();
    run({0xC3000011, 0xA4100400, 0x00000000}, 0, 0, &state); // r1 = statics[0x01]; [main] = r1
    CHECK(at<uint32_t>(main_memory, 0) == 0x44);
    // C0: if r2 (4 bytes) == the value at main + 0x10. The stores add r0, which stays 0.
    clear();
    put<uint32_t>(main_memory, 0x10, 7);
    run({0x40020000, 0x00000000, 0x00000007,
         0xC0452000, 0x00000010,
         0x04000000, 0x00000000, 0x00000001,
         0x20000000,
         0xC0452400, 0x00000008,
         0x04000000, 0x00000004, 0x00000001,
         0x20000000});
    CHECK(at<uint32_t>(main_memory, 0) == 1 && at<uint32_t>(main_memory, 4) == 0);
}

void buttons() {
    clear();
    const std::vector<uint32_t> held = {0x80000011, 0x04000000, 0x00000000, 0x00000001, 0x20000000};
    run(held, 0x10);
    CHECK(at<uint32_t>(main_memory, 0) == 0);
    run(held, 0x11);
    CHECK(at<uint32_t>(main_memory, 0) == 1);
    // C4 without repeat: only on the run where the buttons were just pressed.
    clear();
    const std::vector<uint32_t> edge = {0xC4000000, 0x00000000, 0x00000001, 0x04000000, 0x00000000, 0x00000001, 0x20000000};
    run(edge, 1, 1);
    CHECK(at<uint32_t>(main_memory, 0) == 0);
    run(edge, 1, 0);
    CHECK(at<uint32_t>(main_memory, 0) == 1);
}

void unreadable_instructions() {
    clear();
    // A debug log of each operand form is read past, and does nothing.
    CHECK(!run({0xFFF41000, 0x00000000, 0xFFF41300, 0xFFF44400, 0xFF000000, 0xFF100000,
                0x04000000, 0x00000000, 0x00000001}).stopped);
    CHECK(at<uint32_t>(main_memory, 0) == 1);
    // An unknown type stops the run, at its word.
    const uint16_t owners[] = {cheat::kMaster, 3};
    const uint32_t words[] = {0x20000000, 0xE0000000};
    cheat::State state{};
    cheat::Memory memory{{main_at(), heap_at(), 0, 0}, nullptr, vm_read, vm_write, 0, 0};
    const cheat::Report report = cheat::run(words, owners, 2, memory, state);
    CHECK(report.stopped && report.stopped_at == 1 && report.stopped_owner == 3);
    uint32_t stopped = 0;
    CHECK(!cheat::decodable(words, 2, stopped) && stopped == 1);
    // A width of 3 and words missing at the end can't be read.
    const uint32_t width3[] = {0x03000000, 0x00000000, 0x00000001};
    CHECK(!cheat::decodable(width3, 3, stopped));
    const uint32_t short_store[] = {0x04000000, 0x00000000};
    CHECK(!cheat::decodable(short_store, 2, stopped));
}

// ---- The runtime ----

alignas(8) uint8_t code[256];  // the game's code: readable, not writable
alignas(8) uint8_t data[256];  // memory the game writes
uint64_t code_at() { return reinterpret_cast<uint64_t>(code); }
uint64_t data_at() { return reinterpret_cast<uint64_t>(data); }

std::string cheat_text;
std::string saved_choices;
bool have_saved = false;
bool listed = true;
TSUCHI_Mod* const context = reinterpret_cast<TSUCHI_Mod*>(uintptr_t(0x1000));
std::vector<std::string> notes;

bool host_mod_at(uint32_t index, const char*& id, TSUCHI_Mod*& out) {
    if (!listed || index) return false;
    id = "cheat.mod";
    out = context;
    return true;
}
TSUCHI_Result host_read(const char* id, const char* path, char*& text, uint64_t& size) {
    if (std::strcmp(id, "cheat.mod") || std::strcmp(path, "cheats/0123456789ABCDEF.txt") || cheat_text.empty()) return TSUCHI_NOT_FOUND;
    text = static_cast<char*>(tsuchi::allocate(cheat_text.size()));
    std::memcpy(text, cheat_text.data(), cheat_text.size());
    size = cheat_text.size();
    return TSUCHI_OK;
}
TSUCHI_Result host_load(char*& text, uint64_t& size) {
    if (!have_saved) return TSUCHI_NOT_FOUND;
    text = static_cast<char*>(tsuchi::allocate(saved_choices.size()));
    std::memcpy(text, saved_choices.data(), saved_choices.size());
    size = saved_choices.size();
    return TSUCHI_OK;
}
TSUCHI_Result host_save(const char* text, uint64_t size) {
    saved_choices.assign(text, size);
    have_saved = true;
    return TSUCHI_OK;
}
bool host_regions(uint64_t base[4]) {
    base[0] = code_at();
    base[1] = data_at();
    base[2] = base[3] = 0;
    return true;
}
uint64_t host_keys() { return 0; }
bool host_readable(uint64_t address, uint64_t bytes) {
    return (address >= code_at() && address + bytes <= code_at() + sizeof code) ||
           (address >= data_at() && address + bytes <= data_at() + sizeof data);
}
bool host_writable(uint64_t address, uint64_t bytes) { return address >= data_at() && address + bytes <= data_at() + sizeof data; }
void host_note(const char*, const char* text) { notes.push_back(text); }

// A native backend that patches the test's code array, as the real one patches the game's.
struct Held { uint64_t id, offset; uint32_t original, replacement; bool live; };
std::vector<Held> held;
TSUCHI_Result native_patch(TSUCHI_Mod*, uint64_t offset, uint32_t expected, uint32_t replacement, uint64_t* id) {
    if (offset + 4 > sizeof code) return TSUCHI_INVALID;
    uint32_t there;
    std::memcpy(&there, code + offset, 4);
    if (there != expected) return TSUCHI_BUSY;
    std::memcpy(code + offset, &replacement, 4);
    held.push_back({held.size() + 1, offset, expected, replacement, true});
    *id = held.back().id;
    return TSUCHI_OK;
}
TSUCHI_Result native_remove(TSUCHI_Mod*, uint64_t id) {
    for (Held& h : held)
        if (h.id == id && h.live) {
            uint32_t there;
            std::memcpy(&there, code + h.offset, 4);
            if (there == h.replacement) std::memcpy(code + h.offset, &h.original, 4);
            h.live = false;
            return TSUCHI_OK;
        }
    return TSUCHI_NOT_FOUND;
}
TSUCHI_Result native_none(TSUCHI_Mod*, uint64_t, uint32_t, void*, uint64_t*, void**) { return TSUCHI_NOT_READY; }
TSUCHI_Result native_check(uint64_t, const void*, uint64_t) { return TSUCHI_OK; }
const tsuchi::NativeHooks native{native_none, native_none, native_patch, native_remove, native_check};

void start_runtime() {
    tsuchi::cheats_reset();
    tsuchi::CheatHost host;
    host.mod_at = host_mod_at;
    host.read_file = host_read;
    host.load_choices = host_load;
    host.save_choices = host_save;
    host.regions = host_regions;
    host.keys = host_keys;
    host.readable = host_readable;
    host.writable = host_writable;
    host.code_start = code_at();
    host.code_bytes = sizeof code;
    host.build = "0123456789ABCDEF0011223344556677";
    host.note = host_note;
    tsuchi::set_cheat_host(host);
}

uint32_t code_word(uint64_t offset) {
    uint32_t word;
    std::memcpy(&word, code + offset, 4);
    return word;
}

void runtime() {
    std::memset(code, 0, sizeof code);
    std::memset(data, 0, sizeof data);
    put<uint32_t>(code, 0x20, 0x540008CA); // a branch the cheat replaces
    // The master sets r0; "Money" stores 999 at data + 0x10 every frame; "No limit" turns the branch at code + 0x20
    // into a nop; "Broken" has a word no interpreter knows.
    cheat_text = "{Setup}\n40000000 00000000 00000010\n"
                 "[Money]\n04100000 00000000 000003E7\n"
                 "[No limit]\n04000000 00000010 D503201F\n"
                 "[Broken]\nE0000000\n";
    tsuchi::set_native_hooks(&native);
    start_runtime();
    tsuchi::cheats_frame();
    CHECK(tsuchi::cheat_count() == 3);
    tsuchi::CheatEntry entry{};
    CHECK(tsuchi::cheat_at(0, entry) && !std::strcmp(entry.name, "Money") && !entry.on && !entry.problem[0]);
    CHECK(tsuchi::cheat_at(2, entry) && entry.problem[0]);
    CHECK(tsuchi::set_cheat("cheat.mod", "Broken", true) == TSUCHI_UNSUPPORTED);
    CHECK(tsuchi::set_cheat("cheat.mod", "Nothing", true) == TSUCHI_NOT_FOUND);
    tsuchi::cheats_frame();
    CHECK(at<uint32_t>(data, 0x10) == 0); // everything starts off

    // On: the data write happens each frame, the code write becomes a patch.
    CHECK(tsuchi::set_cheat("cheat.mod", "Money", true) == TSUCHI_OK);
    CHECK(tsuchi::set_cheat("cheat.mod", "No limit", true) == TSUCHI_OK);
    tsuchi::cheats_frame();
    CHECK(at<uint32_t>(data, 0x10) == 999);
    CHECK(code_word(0x20) == 0xD503201F && tsuchi::cheat_stats().patches == 1);
    put<uint32_t>(data, 0x10, 5);
    tsuchi::cheats_frame();
    CHECK(at<uint32_t>(data, 0x10) == 999 && held.size() == 1); // the patch is held, not made again
    CHECK(saved_choices.find("\"cheat.mod\":[\"Money\",\"No limit\"]") != std::string::npos);

    // Off: the code gets its own instruction back at once; the data keeps what the cheat wrote, and is free again.
    CHECK(tsuchi::set_cheat("cheat.mod", "No limit", false) == TSUCHI_OK);
    CHECK(code_word(0x20) == 0x540008CA && tsuchi::cheat_stats().patches == 0);
    CHECK(tsuchi::set_cheat("cheat.mod", "Money", false) == TSUCHI_OK);
    put<uint32_t>(data, 0x10, 5);
    tsuchi::cheats_frame();
    CHECK(at<uint32_t>(data, 0x10) == 5);

    // The choices come back at the next start.
    CHECK(tsuchi::set_cheat("cheat.mod", "Money", true) == TSUCHI_OK);
    start_runtime();
    tsuchi::cheats_frame();
    CHECK(tsuchi::cheat_at(0, entry) && entry.on);
    tsuchi::cheats_frame();
    CHECK(at<uint32_t>(data, 0x10) == 999);

    // Without code access, a cheat that writes code is turned off and says why.
    tsuchi::set_native_hooks(nullptr);
    CHECK(tsuchi::set_cheat("cheat.mod", "No limit", true) == TSUCHI_OK);
    tsuchi::cheats_frame();
    CHECK(tsuchi::cheat_at(1, entry) && !entry.on && std::strstr(entry.problem, "code access"));
    CHECK(code_word(0x20) == 0x540008CA);
    CHECK(tsuchi::cheat_at(0, entry) && entry.on); // the other cheat goes on running
    CHECK(tsuchi::set_cheat("cheat.mod", "No limit", true) == TSUCHI_UNSUPPORTED);

    // A mod that stops takes its cheats with it.
    listed = false;
    tsuchi::cheats_frame();
    CHECK(tsuchi::cheat_count() == 0);
    listed = true;
    tsuchi::set_native_hooks(&native);

    // A file that breaks a rule is told in the log, and gives no cheats.
    cheat_text = "[A] 20000000\n";
    start_runtime();
    notes.clear();
    tsuchi::cheats_frame();
    CHECK(tsuchi::cheat_count() == 0 && !notes.empty());
}

} // namespace

int main() {
    fake::start();
    parsing();
    stores_and_loads();
    conditions_and_loops();
    arithmetic_and_registers();
    buttons();
    unreadable_instructions();
    runtime();
    if (failures) std::printf("%d failures\n", failures);
    else std::puts("cheats passed");
    return failures;
}
