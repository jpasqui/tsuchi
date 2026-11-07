// Focused adapter policy checks against a reflected save stand-in. This does not
// qualify native constructors, serializers, or hook timing in the game.
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "pikmin_save.h"
#include "reflect.h"

namespace {
struct Node {
    enum Kind { structure, array, integer, flag } kind = structure;
    std::map<std::string, uint64_t> fields;
    std::vector<uint64_t> rows;
    uint64_t prototype = 0;
    int64_t value = 0;
};
std::map<uint64_t, Node> nodes;
uint64_t next_id = 1;
int allocations = 0, fail_after = -1, failures = 0;
#define EXPECT(x) do { if (!(x)) { std::printf("%d: %s\n", __LINE__, #x); ++failures; } } while (0)
uint64_t make(Node::Kind kind = Node::structure) { uint64_t id = next_id++; nodes[id].kind = kind; return id; }
uint64_t copy(uint64_t id) {
    Node source = nodes.at(id);
    const uint64_t out = make(source.kind);
    nodes[out].value = source.value;
    nodes[out].prototype = source.prototype;
    for (const auto& field : source.fields) nodes[out].fields[field.first] = copy(field.second);
    for (const auto row : source.rows) nodes[out].rows.push_back(copy(row));
    return out;
}
uint64_t number_row(unsigned count, const char* field) {
    uint64_t row = make(), array = make(Node::array);
    nodes[row].fields[field] = array;
    nodes[array].prototype = make(Node::integer);
    for (unsigned i = 0; i < count; ++i) nodes[array].rows.push_back(make(Node::integer));
    return row;
}
uint64_t array(uint64_t prototype, unsigned count) {
    uint64_t id = make(Node::array);
    nodes[id].prototype = prototype;
    for (unsigned i = 0; i < count; ++i) nodes[id].rows.push_back(copy(prototype));
    return id;
}
uint64_t result(unsigned count) {
    uint64_t id = make();
    nodes[id].fields["UnionPikminNum"] = array(number_row(4, "Num"), count);
    nodes[id].fields["DeadPikminNum"] = array(number_row(4, "Num"), count);
    return id;
}
uint64_t summary(unsigned count) {
    uint64_t id = make();
    for (const char* f : {"FollowPikminNum0", "FollowPikminNum1", "OnyonPikminNum"})
        nodes[id].fields[f] = array(number_row(4, "Num"), count);
    for (const char* f : {"OnyonDecoPikminNum", "OutSideOnyonDecoPikminNum"})
        nodes[id].fields[f] = array(number_row(25, "DecoNum"), count);
    nodes[id].fields["TotalStoryResultInfo"] = result(count);
    return id;
}
uint64_t story(unsigned count) {
    uint64_t id = make(), flag = make();
    nodes[flag].fields["bFlag"] = make(Node::flag);
    nodes[id].fields["StorySummary"] = summary(count);
    nodes[id].fields["PreviousStorySummary"] = summary(count);
    nodes[id].fields["LocalStoryResultInfo"] = array(result(count), 3);
    nodes[id].fields["UnionPikmin"] = array(flag, count);
    nodes[id].fields["UniteOnyon"] = array(flag, count);
    return id;
}
uint64_t field(uint64_t id, const char* f) { return nodes.at(id).fields.at(f); }
uint64_t counter(uint64_t sum, const char* f, unsigned color, unsigned leaf = 0) {
    const uint64_t row = nodes.at(field(sum, f)).rows.at(color);
    return nodes.at(field(row, std::string(f).find("Deco") == std::string::npos ? "Num" : "DecoNum")).rows.at(leaf);
}
}
namespace tsuchi::game {
uint64_t type_of(uint64_t id) { return nodes.count(id) ? id : 0; }
TSUCHI_Result find_place(uint64_t, uint64_t base, const char* path, Place& out) {
    std::string name(path);
    const size_t bracket = name.find('[');
    const std::string field_name = name.substr(0, bracket);
    auto node = nodes.find(base);
    if (node == nodes.end() || !node->second.fields.count(field_name)) return TSUCHI_NOT_FOUND;
    uint64_t id = node->second.fields.at(field_name);
    if (bracket != std::string::npos) {
        const unsigned index = unsigned(std::stoul(name.substr(bracket + 1)));
        if (index >= nodes[id].rows.size()) return TSUCHI_NOT_FOUND;
        id = nodes[id].rows[index];
    }
    out = Place{id, id, false};
    return TSUCHI_OK;
}
TSUCHI_Result array_of(const Place& p, ArrayMemory& out) {
    if (!nodes.count(p.address) || nodes[p.address].kind != Node::array) return TSUCHI_INVALID;
    out = ArrayMemory{p.address, int32_t(nodes[p.address].rows.size()), int32_t(nodes[p.address].rows.size())};
    return TSUCHI_OK;
}
TSUCHI_Result resize(const Place& p, uint32_t count) {
    if (fail_after == allocations) return TSUCHI_NO_MEMORY;
    ++allocations;
    Node& a = nodes[p.address];
    while (a.rows.size() < count) a.rows.push_back(copy(a.prototype));
    a.rows.resize(count);
    return TSUCHI_OK;
}
TSUCHI_Result struct_at(const Place& p, uint64_t& type, uint64_t& base) {
    if (nodes[p.address].kind != Node::structure) return TSUCHI_INVALID;
    type = base = p.address;
    return TSUCHI_OK;
}
TSUCHI_Result read_value(const Place& p, Value& v) {
    const Node& n = nodes.at(p.address);
    v.kind = n.kind == Node::integer ? ValueKind::integer : ValueKind::none;
    v.integer = n.value;
    return TSUCHI_OK;
}
TSUCHI_Result write_value(const Place& p, const Value& v) {
    Node& n = nodes[p.address];
    if (n.kind == Node::integer && v.kind == ValueKind::integer) n.value = v.integer;
    else if (n.kind == Node::flag && v.kind == ValueKind::boolean) n.value = v.boolean;
    else return TSUCHI_INVALID;
    return TSUCHI_OK;
}
}
int main() {
    using tsuchi::pikmin_save_story;
    const uint64_t save = story(17), sum = field(save, "StorySummary"), previous = field(save, "PreviousStorySummary");
    nodes[counter(sum, "OnyonPikminNum", 0, 3)].value = 123;
    EXPECT(pikmin_save_story(save) == TSUCHI_OK);
    EXPECT(nodes[field(sum, "OnyonPikminNum")].rows.size() == 22);
    EXPECT(nodes[field(previous, "OnyonDecoPikminNum")].rows.size() == 22);
    EXPECT(nodes[counter(sum, "OnyonPikminNum", 0, 3)].value == 123);
    EXPECT(nodes[counter(sum, "OnyonPikminNum", 21, 3)].value == 0);
    EXPECT(nodes[counter(previous, "OnyonDecoPikminNum", 19, 24)].value == 0);
    EXPECT(nodes[field(save, "LocalStoryResultInfo")].rows.size() == 3);
    const uint64_t day = nodes[field(save, "LocalStoryResultInfo")].rows[2];
    EXPECT(nodes[field(day, "DeadPikminNum")].rows.size() == 22);
    const uint64_t unlock = field(nodes[field(save, "UnionPikmin")].rows[19], "bFlag");
    const uint64_t onion = field(nodes[field(save, "UniteOnyon")].rows[21], "bFlag");
    nodes[unlock].value = nodes[onion].value = 1;
    const uint64_t stock_flag = field(nodes[field(save, "UnionPikmin")].rows[0], "bFlag");
    nodes[stock_flag].value = 1;
    nodes[counter(day, "DeadPikminNum", 21, 3)].value = 99;
    nodes[counter(sum, "OnyonPikminNum", 19)].value = 44;
    nodes[counter(previous, "OnyonDecoPikminNum", 21, 24)].value = 55;
    const int before = allocations;
    EXPECT(pikmin_save_story(save) == TSUCHI_OK && allocations == before);
    EXPECT(nodes[counter(sum, "OnyonPikminNum", 19)].value == 44);
    EXPECT(nodes[unlock].value == 1 && nodes[onion].value == 1);
    EXPECT(nodes[counter(day, "DeadPikminNum", 21, 3)].value == 99);
    EXPECT(pikmin_save_story(save, true) == TSUCHI_OK);
    EXPECT(nodes[counter(sum, "OnyonPikminNum", 19)].value == 0);
    EXPECT(nodes[unlock].value == 0 && nodes[onion].value == 0);
    EXPECT(nodes[stock_flag].value == 1);
    EXPECT(nodes[counter(day, "DeadPikminNum", 21, 3)].value == 0);
    EXPECT(nodes[counter(previous, "OnyonDecoPikminNum", 21, 24)].value == 0);
    EXPECT(nodes[counter(sum, "OnyonPikminNum", 0, 3)].value == 123);
    const uint64_t future = story(30), future_sum = field(future, "StorySummary");
    nodes[counter(future_sum, "OnyonPikminNum", 29)].value = 66;
    const uint64_t unknown_row = nodes[field(future_sum, "OnyonPikminNum")].rows[29];
    const uint64_t unknown_counts = field(unknown_row, "Num");
    nodes[unknown_counts].rows.push_back(make(Node::integer));
    nodes[unknown_counts].rows.push_back(make(Node::integer));
    nodes[nodes[unknown_counts].rows[5]].value = 77;
    EXPECT(pikmin_save_story(future) == TSUCHI_OK);
    EXPECT(nodes[field(future_sum, "OnyonPikminNum")].rows.size() == 30);
    EXPECT(nodes[counter(future_sum, "OnyonPikminNum", 29)].value == 66);
    EXPECT(nodes[unknown_counts].rows.size() == 6 && nodes[nodes[unknown_counts].rows[5]].value == 77);
    const uint64_t clear_save = story(30), clear_sum = field(clear_save, "StorySummary");
    const uint64_t clear_previous = field(clear_save, "PreviousStorySummary");
    std::vector<uint64_t> target;
    for (const char* f : {"FollowPikminNum0", "FollowPikminNum1"})
        for (unsigned color : {19u, 21u})
            for (unsigned leaf = 0; leaf < 4; ++leaf) target.push_back(counter(clear_sum, f, color, leaf));
    for (auto& pair : nodes) if (pair.second.kind == Node::integer) pair.second.value = 37;
    const auto all_before = nodes;
    const int allocations_before_clear = allocations;
    EXPECT(tsuchi::pikmin_save_clear_follow(clear_save) == TSUCHI_OK);
    EXPECT(allocations == allocations_before_clear);
    for (const auto& pair : all_before) {
        bool changed = false;
        for (uint64_t id : target) if (id == pair.first) changed = true;
        EXPECT(nodes.at(pair.first).value == (changed ? 0 : pair.second.value));
        EXPECT(nodes.at(pair.first).rows == pair.second.rows);
        EXPECT(nodes.at(pair.first).fields == pair.second.fields);
    }
    EXPECT(nodes[counter(clear_previous, "FollowPikminNum0", 19)].value == 37);
    EXPECT(tsuchi::pikmin_save_clear_follow(clear_save) == TSUCHI_OK);
    nodes[target[0]].value = 42;
    const auto second_outer = field(clear_sum, "FollowPikminNum1");
    const auto saved_rows = nodes[second_outer].rows;
    nodes[second_outer].rows.resize(21);
    EXPECT(tsuchi::pikmin_save_clear_follow(clear_save) == TSUCHI_NOT_READY);
    EXPECT(nodes[target[0]].value == 42);
    nodes[second_outer].rows = saved_rows;
    const auto last_num = field(nodes[second_outer].rows[21], "Num");
    const auto saved_counts = nodes[last_num].rows;
    nodes[last_num].rows.resize(3);
    EXPECT(tsuchi::pikmin_save_clear_follow(clear_save) == TSUCHI_NOT_READY);
    EXPECT(nodes[target[0]].value == 42);
    nodes[last_num].rows = saved_counts;
    nodes[target.back()].kind = Node::flag;
    EXPECT(tsuchi::pikmin_save_clear_follow(clear_save) == TSUCHI_INVALID);
    EXPECT(nodes[target[0]].value == 42);
    nodes[target.back()].kind = Node::integer;
    EXPECT(tsuchi::pikmin_save_clear_follow(clear_save) == TSUCHI_OK);
    EXPECT(tsuchi::pikmin_save_clear_follow(0) == TSUCHI_NOT_READY);
    const uint64_t failed = story(17);
    fail_after = allocations + 2;
    EXPECT(pikmin_save_story(failed) == TSUCHI_NO_MEMORY);
    fail_after = -1;
    EXPECT(pikmin_save_story(failed) == TSUCHI_OK);
    EXPECT(pikmin_save_story(0) == TSUCHI_NOT_READY);
    nodes[save].fields.erase("PreviousStorySummary");
    EXPECT(pikmin_save_story(save) == TSUCHI_NOT_FOUND);
    std::printf("Pikmin save adapter policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
