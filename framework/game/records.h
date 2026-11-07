// The records the framework itself keeps in the player's save: which number of the game's each thing a mod adds
// was given. DESIGN.md, "The framework's records", has the rule, which is the same for every record:
//
//   - A record is kept as two copies in the framework's own folder of the save, `<name>.json` and
//     `<name>.2.json`. Each write goes to the copy that was not written last, with a counter one higher, and is
//     whole and kept by the save before the other copy is touched.
//   - It is read once in a run, by whoever needs it first: the copy with the highest counter that can be
//     understood. From then on what is in memory is the record.
//   - A member no part of this framework knows is kept as it is written and passed over: a later version may
//     have put it there.
//   - With copies there and none that can be understood, the record is damaged: nothing is given out and
//     nothing is written over it.
//
// A service keeps what the record holds in memory of its own, and gives this the two steps that pass between
// that memory and a copy's text.
//
// Game thread only.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "tsuchi/core.h"

namespace tsuchi {

// Text put together piece by piece, in room that grows.
struct Written {
    char* data = nullptr;
    size_t used = 0, capacity = 0;
    bool whole = true; // false once there was no memory for a piece

    Written& add(const char* text);
    Written& add(const char* text, size_t bytes);
    Written& number(uint64_t value);
    // Gives the room back.
    void clear();
};

// The members of an object, each as it was read and as it is written in the copy it was read from. A service
// uses it to keep what it does not know in an object of its own.
struct Members {
    // `text` is where the object is written: at its opening brace, or at blank space before it.
    Members(const Json& object, const char* text);
    // The next member. False after the last.
    bool next();

    const Json* value = nullptr;      // as it was read: `value->name` is its name
    const char* text = nullptr;       // as it is written, whole: its name, the colon and its value
    size_t bytes = 0;
    const char* value_text = nullptr; // where its value is written

  private:
    const Json* next_;
    const char* at_;
};

struct Record {
    // ---- What its service says of it ----
    const char* name;         // the copies are `<name>.json` and `<name>.2.json`
    const char* old_place;    // a file an earlier version of the framework kept it in, by its whole path; or null
    const char* const* known; // the members its service reads and writes
    uint32_t known_count;
    // Fills the service's memory from a copy: the object that was read, and where it is written. False for a copy
    // in which something the service knows is not as it should be.
    bool (*take)(const Json& root, const char* text);
    // Adds each member the service knows to a copy that is being written, with `, ` before each.
    void (*put)(Written& out);
    // Empties the service's memory of it.
    void (*clear)();

    // ---- What this run knows of it ----
    enum class State : uint8_t { unread, read, damaged } state = State::unread;
    enum class Source : uint8_t { none, first, second, old_place } source = Source::none; // the copy memory was last read from or written to
    uint64_t counter = 0;                                                      // that copy's
    Written others{};                                                        // members no service knows, as they are written
    // Since the framework started: copies understood, copies that could not be, members kept and passed over,
    // writes the save took, and writes it refused.
    uint32_t reads = 0, damaged = 0, strangers = 0, writes = 0, refused = 0;
    Record* next = nullptr; // every record that has been asked for, for `forget_records`
};

// Has the record in the service's memory: read now, or earlier in this run. TSUCHI_INVALID for a record that is
// damaged, which it stays for the run. What reading the save answered when it could not be read, and then the
// next call reads again.
TSUCHI_Result read_record(Record& record);
// Writes what the service's memory holds as the record's next copy. Whatever the save answered; a write it
// refused leaves the copy that was written last as it is. TSUCHI_NOT_READY for a record that was not read.
TSUCHI_Result write_record(Record& record);
// Forgets what was read, as when the game starts. In the game a run begins once; the checks begin several in
// one process.
void forget_records();

} // namespace tsuchi
