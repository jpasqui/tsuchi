// The game's objects: which are alive, handles to them, and their classes.
#pragma once
#include <stdint.h>

#include "names.h"

namespace tsuchi::game {

// ---- The object table ------------------------------------------------------

// True for an object the game currently has and is not destroying.
bool live(uint64_t object);
uint64_t class_of(uint64_t object);
uint64_t outer_of(uint64_t object);
Name name_of(uint64_t object);
// Calls `visit` for every live object until it returns false.
void each_object(bool (*visit)(void* context, uint64_t object), void* context);
// The same in pieces: from the table's place `next` on, until `visit` returns false or the table ends. `next`
// is left at the first place not visited. True once the end of the table has been reached.
bool each_object_from(int32_t& next, bool (*visit)(void* context, uint64_t object), void* context);

// An object's path as UTF-8 with a terminator: its package first, then each
// object inside the one before. The engine writes a colon after the object
// that sits in the package, and dots elsewhere. `needed` gets the bytes it
// takes; TSUCHI_CAPACITY when that is more than `capacity`, and nothing is
// written. TSUCHI_NOT_FOUND when a name cannot be read.
TSUCHI_Result object_path(uint64_t object, char* buffer, uint64_t capacity, uint64_t* needed);
// True for an object that is a class.
bool is_class(uint64_t object);

// ---- Handles ---------------------------------------------------------------
//
// A handle names one life of one object. It stops resolving when the game
// destroys the object, when another object takes its place in the table, and
// when the game reuses the same object for something else (see end_life).
// Zero is never a handle.

uint64_t to_handle(uint64_t object);   // 0 for an object that is not live
uint64_t from_handle(uint64_t handle); // the object, or 0 once that life is over
// The game keeps some objects and reuses them, such as a Pikmin that goes back
// to its pool. Whoever learns that an object's current use has ended calls
// this, and every handle made before stops resolving.
void end_life(uint64_t object);
// The object the game's own weak reference at `at` (a number and a serial) leads to, or 0.
uint64_t from_weak_reference(uint64_t at);
// The same for a weak reference held in one value: the number in its low half, the serial in its high half.
uint64_t from_weak_value(uint64_t packed);
// The game's weak reference to an object, in one value; to nothing, for an object that is not live.
uint64_t to_weak_value(uint64_t object);

// Tells the engine to keep an object whatever else lets go of it, or stops
// telling it so. True when this call changed what the engine was told: the
// object was not being kept before, or was.
bool keep_object(uint64_t object, bool keep);
bool object_kept(uint64_t object);

// ---- Classes ---------------------------------------------------------------

// A class looked up by name once and remembered. Make one static ClassRef per
// class a service asks about.
struct ClassRef {
    const char* name;
    uint64_t found = 0, handle = 0;
};
// The class object, or 0 while the game has no class of that name loaded.
uint64_t find_class(ClassRef& ref);
// True when the object's class is the class or derives from it.
bool is_a(uint64_t object, ClassRef& ref);
// The object a member of `object` refers to, the member found by name among its class's fields (an ObjectProperty):
// for a member a Blueprint declares, whose place each cooking of the class decides. 0 for none.
uint64_t member_object(uint64_t object, const char* name);

} // namespace tsuchi::game
