// IDs: the names of kinds of thing, where mods can add kinds. DESIGN.md, "IDs", has the rules.
//
// A thing a mod adds is `<mod id>:<name>`. A thing of the game's own has a plain name with no
// colon. A number the game has that nobody has named is `#` and the number. Every service that
// takes or gives an ID reads it with the functions here, so an ID is spelled one way everywhere.
//
// Game thread only.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "list.h"
#include "registry.h"

namespace tsuchi {

// Room for the longest ID and its terminator: a mod's ID, a colon and a name, 47 + 1 + 47 bytes.
// TSUCHI_ID_SIZE in the public header is the same number.
constexpr size_t kIdBytes = 96;
static_assert(kIdBytes == TSUCHI_ID_SIZE, "an ID in a struct a mod is given has room for the longest one");
// The longest plain name.
constexpr size_t kIdLength = kIdBytes - 1;

// How an ID is spelled. Nothing here tells whether it names anything.
enum class IdForm : uint8_t {
    malformed,   // not an ID
    plain,       // the game's own: 1 to 95 of A-Z, a-z, 0-9, '.', '_' and '-'
    added,       // a mod's: `<mod id>:<name>`, each part spelled as a mod's ID is
    placeholder, // `#` and a number from 0 to 4294967295, with no zeros in front
};
// Reads an ID's spelling. For `added`, `colon` gets where the colon is. For `placeholder`, `number`
// gets the number. Text with no terminator in its first kIdBytes bytes is malformed, and nothing
// past them is read.
IdForm id_form(const char* id, size_t* colon = nullptr, uint32_t* number = nullptr);
inline bool id_valid(const char* id) { return id_form(id) != IdForm::malformed; }

// True when both are IDs and name the same thing. Letter case counts for nothing in a plain
// name, as in the engine's own names; the other forms have one spelling each.
bool id_same(const char* a, const char* b);

// Writes `<mod id>:<name>`. False when either part is not spelled as a mod's ID is.
bool id_join(const char* mod, const char* name, char out[kIdBytes]);
// Splits `<mod id>:<name>`: `mod` gets the first part and `name` points at the second, inside
// `id`. False for anything else.
bool id_split(const char* id, char mod[kModIdBytes], const char** name);
// Writes the placeholder for a number.
void id_placeholder(uint32_t number, char out[kIdBytes]);

// ---- A vocabulary the game keeps as numbers ---------------------------------------------

// One of the game's own that the framework has a name for.
struct IdName {
    const char* name;
    uint32_t value;
};

// A set of kinds the game tells apart by number, such as the types of Pikmin. The game's own
// come from the schema. What mods add is recorded here, by whichever service adds it, and goes
// when its mod stops. One static Vocabulary per set; the generated service code has them.
struct Vocabulary {
    const char* name;  // "pikmin.PikminType"
    const IdName* own; // the game's own, by the names the schema gives them
    uint32_t own_count;
    uint32_t most; // the largest number the game can hold for one

    struct Added {
        char id[kIdBytes];
        uint32_t value;
        uint64_t key; // what the mod's record of it is kept under
    };
    List<Added> added = {}; // what mods have added
    // Numbers given to content stay tied to that ID for the process. Some game
    // rows survive their owner, so a later mod must not inherit their number.
    struct Given { char id[kIdBytes]; uint32_t value; };
    List<Given> given = {};
};

// The number an ID stands for. TSUCHI_INVALID for text that is not an ID. TSUCHI_NOT_FOUND for an
// ID that names nothing here: a name nobody has, a mod's thing whose mod is not running, or a
// placeholder past what the game can hold. A placeholder within that is always taken, so what
// `id_of` gave can be given back.
TSUCHI_Result id_value(const Vocabulary& vocabulary, const char* id, uint32_t* out);
// The ID of a number: its name, or its placeholder when it has none. There is always one.
void id_of(const Vocabulary& vocabulary, uint32_t value, char out[kIdBytes]);
// The same into a caller's buffer, as text crosses to a mod: TSUCHI_CAPACITY when it does not
// fit, with nothing copied and `out` still set.
TSUCHI_Result id_into(const Vocabulary& vocabulary, uint32_t value, char* buffer, uint64_t capacity, uint64_t* out);
// True when the number has a name: the game's own or a running mod's.
bool id_named(const Vocabulary& vocabulary, uint32_t value);

// Names a number `<mod id>:<name>` for as long as the mod runs. The service that adds a mod's
// thing to the game calls this with the number it gave the thing; a mod never picks one.
// TSUCHI_INVALID for a name that is not spelled right or a number past what the game can hold,
// TSUCHI_BUSY when the number or the ID already names something, TSUCHI_NO_MEMORY.
TSUCHI_Result id_add(Vocabulary& vocabulary, TSUCHI_Mod* mod, const char* name, uint32_t value);
// Takes a name back before its mod stops. TSUCHI_NOT_FOUND when the mod added no such name.
TSUCHI_Result id_remove(Vocabulary& vocabulary, TSUCHI_Mod* mod, const char* name);

// Gives the mod's name the first unused number in the admitted inclusive range.
// Reuses the same ID's allocation after its mod restarts. Numbers are never
// given to another ID during this process, even after id_remove or mod_stop.
// A backend chooses a range the native game supports and reserves stock values.
// TSUCHI_CAPACITY when the range is full. No allocation is retained on failure.
TSUCHI_Result id_allocate(Vocabulary& vocabulary, TSUCHI_Mod* mod, const char* name,
                          uint32_t first, uint32_t last, uint32_t* out);

} // namespace tsuchi
