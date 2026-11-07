#include "records.h"

#include <string.h>

#include "registry.h"
#include "save_session.h"
#include "services.h"
#include "texts.h"

namespace tsuchi {
namespace {

// The framework's own folder in the save. No mod has that name: names that begin with `tsuchi.` are the framework's.
constexpr char kFolder[] = "/tsuchi.content/";
constexpr char kCounter[] = "counter";
Record* records = nullptr;

// ---- Text that was read as JSON, gone through as it is written --------------------------------------
//
// The reader took the text, so every text in it is closed and every bracket has its match: these stop.

bool blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
const char* past_blank(const char* at) {
    while (blank(*at)) ++at;
    return at;
}
// From a text's opening quote to past its closing one.
const char* past_text(const char* at) {
    for (++at; *at != '"'; ++at)
        if (*at == '\\') ++at;
    return at + 1;
}
const char* past_value(const char* at) {
    if (*at == '"') return past_text(at);
    if (*at != '{' && *at != '[') { // a number or a word: up to what ends it
        while (*at != ',' && *at != '}' && *at != ']' && !blank(*at)) ++at;
        return at;
    }
    int depth = 0;
    do {
        if (*at == '"') {
            at = past_text(at);
            continue;
        }
        depth += *at == '{' || *at == '[' ? 1 : *at == '}' || *at == ']' ? -1 : 0;
        ++at;
    } while (depth);
    return at;
}

bool path_of(Texts& out, const Record& record, Record::Source copy) {
    if (copy == Record::Source::old_place) return out.add(record.old_place);
    return out.add(game::kSavedMods, kFolder, record.name, copy == Record::Source::first ? ".json" : ".2.json");
}

// One copy as the save has it.
struct Copy {
    Record::Source which;
    char* text = nullptr;
    uint64_t bytes = 0;
    const Json* root = nullptr; // null for a copy that is not there, or cannot be understood
    uint64_t counter = 0;
    bool there = false;
};

// Reads a copy. Anything but TSUCHI_OK when the save could not be asked: a copy that is not there is no failure.
TSUCHI_Result fetch(const Record& record, Copy& copy) {
    Texts where;
    if (!path_of(where, record, copy.which)) return TSUCHI_NO_MEMORY;
    const TSUCHI_Result result = read_save_file(where.data, copy.text, copy.bytes);
    where.clear();
    if (result == TSUCHI_NOT_FOUND) return TSUCHI_OK;
    if (result != TSUCHI_OK) return result;
    copy.there = true;
    // A copy is an object. Its counter is a whole number, and 0 for one written before copies were counted.
    copy.root = read_json(copy.text, size_t(copy.bytes));
    const Json* counter = copy.root && copy.root->kind == Json::Kind::object ? copy.root->find(kCounter) : nullptr;
    const bool sound = copy.root && copy.root->kind == Json::Kind::object && (!counter || (counter->kind == Json::Kind::number && counter->whole && counter->integer >= 0));
    if (sound) copy.counter = counter ? uint64_t(counter->integer) : 0;
    else release_json(copy.root), copy.root = nullptr;
    return TSUCHI_OK;
}

void drop(Copy& copy) {
    release_json(copy.root), release(copy.text);
    copy.root = nullptr, copy.text = nullptr;
}

bool knows(const Record& record, const char* name) {
    if (!strcmp(name, kCounter)) return true;
    for (uint32_t i = 0; i < record.known_count; ++i)
        if (!strcmp(name, record.known[i])) return true;
    return false;
}

// Keeps the members of a copy that no service knows, as they are written, each with `, ` before it.
void keep_others(Record& record, const Copy& copy) {
    record.others.clear();
    Members members(*copy.root, copy.text);
    while (members.next()) {
        if (knows(record, members.value->name)) continue;
        record.others.add(", ").add(members.text, members.bytes);
        ++record.strangers;
    }
}

} // namespace

Written& Written::add(const char* text, size_t bytes) {
    if (!whole) return *this;
    if (used + bytes + 1 > capacity) {
        size_t next = capacity ? capacity * 2 : 256;
        while (next < used + bytes + 1) next *= 2;
        auto* grown = static_cast<char*>(allocate(next));
        if (!grown) return whole = false, *this;
        if (used) memcpy(grown, data, used);
        release(data);
        data = grown, capacity = next;
    }
    if (bytes) memcpy(data + used, text, bytes);
    used += bytes;
    data[used] = 0;
    return *this;
}

Written& Written::add(const char* text) { return add(text, strlen(text)); }

Written& Written::number(uint64_t value) {
    char digits[24];
    char* at = digits + sizeof digits;
    do *--at = char('0' + value % 10);
    while (value /= 10);
    return add(at, size_t(digits + sizeof digits - at));
}

void Written::clear() {
    release(data);
    *this = Written{};
}

Members::Members(const Json& object, const char* text) : next_(object.first), at_(past_blank(text) + 1) {}

bool Members::next() {
    if (!next_) return false;
    at_ = past_blank(at_);
    if (*at_ == ',') at_ = past_blank(at_ + 1);
    text = at_;
    at_ = past_blank(past_text(at_)); // its name, and then the colon
    value_text = at_ = past_blank(at_ + 1);
    at_ = past_value(at_);
    bytes = size_t(at_ - text);
    value = next_, next_ = next_->next;
    return true;
}

TSUCHI_Result read_record(Record& record) {
    if (record.state == Record::State::read) return TSUCHI_OK;
    if (record.state == Record::State::damaged) return TSUCHI_INVALID;
    bool listed = false;
    for (const Record* each = records; each && !listed; each = each->next) listed = each == &record;
    if (!listed) record.next = records, records = &record;

    Copy copies[3] = {{Record::Source::first}, {Record::Source::second}, {Record::Source::old_place}};
    TSUCHI_Result result = fetch(record, copies[0]);
    if (result == TSUCHI_OK) result = fetch(record, copies[1]);
    // What an earlier version kept is read only for a record that has no copy in its own place yet.
    if (result == TSUCHI_OK && record.old_place && !copies[0].there && !copies[1].there) result = fetch(record, copies[2]);
    if (result != TSUCHI_OK) {
        for (Copy& copy : copies) drop(copy);
        return result;
    }
    // The newest copy that can be understood: by the reader, and then by the service.
    const Copy* chosen = nullptr;
    bool any = false;
    for (const Copy& copy : copies) {
        any = any || copy.there;
        record.damaged += copy.there && !copy.root;
    }
    while (!chosen) {
        Copy* newest = nullptr;
        for (Copy& copy : copies)
            if (copy.root && (!newest || copy.counter > newest->counter)) newest = &copy;
        if (!newest) break;
        record.clear();
        if (record.take(*newest->root, newest->text)) {
            chosen = newest;
            break;
        }
        record.clear();
        ++record.damaged;
        release_json(newest->root), newest->root = nullptr;
    }
    record.others.clear();
    record.counter = 0, record.source = Record::Source::none;
    if (chosen) {
        keep_others(record, *chosen);
        record.counter = chosen->counter, record.source = chosen->which;
        ++record.reads;
    }
    const bool whole = record.others.whole;
    for (Copy& copy : copies) drop(copy);
    if (!whole) { // with no memory for what has to be kept, nothing is known yet
        record.clear(), record.others.clear();
        return TSUCHI_NO_MEMORY;
    }
    // A save with no copy at all has an empty record. One with copies and none that can be understood is damaged.
    record.state = chosen || !any ? Record::State::read : Record::State::damaged;
    return record.state == Record::State::read ? TSUCHI_OK : TSUCHI_INVALID;
}

TSUCHI_Result write_record(Record& record) {
    if (record.state != Record::State::read) return TSUCHI_NOT_READY;
    if(record.counter>=uint64_t(INT64_MAX)) return ++record.refused,TSUCHI_CAPACITY;
    const Record::Source to = record.source == Record::Source::first ? Record::Source::second : Record::Source::first;
    Written out;
    out.add("{\"").add(kCounter).add("\": ").number(record.counter + 1);
    record.put(out);
    out.add(record.others.data ? record.others.data : "", record.others.used).add("}\n");
    Texts where;
    TSUCHI_Result result = out.whole && path_of(where, record, to) ? TSUCHI_OK : TSUCHI_NO_MEMORY;
    if (result == TSUCHI_OK) result = write_save_file(where.data, out.data, out.used);
    out.clear(), where.clear();
    if (result != TSUCHI_OK) return ++record.refused, result;
    ++record.counter, ++record.writes;
    record.source = to;
    return TSUCHI_OK;
}

void forget_records() {
    for (Record* record = records; record; record = record->next) {
        record->clear(), record->others.clear();
        record->state = Record::State::unread, record->source = Record::Source::none, record->counter = 0;
    }
}

} // namespace tsuchi
