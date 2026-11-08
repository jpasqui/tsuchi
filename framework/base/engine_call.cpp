#include "engine_call.h"

#include <string.h>

#include "objects.h"

namespace tsuchi::game {

EngineCall::EngineCall(uint64_t object, const char* function) : EngineCall(object, function, {}) {}

EngineCall::EngineCall(uint64_t object, const char* function, CallGuard guard) : object_(0), state_(TSUCHI_NOT_READY), guard_(guard) {
    if (!guard_.permits()) return;
    object_ = to_handle(object);
    if (!guard_.permits()) return;
    const uint64_t type = is_class(object) ? object : class_of(object);
    function_ = object_ ? (guard_.allowed ? find_function_guarded(type, function, guard_) : find_function(type, function)) : 0;
    if (!guard_.permits()) return;
    state_ = function_ ? (guard_.allowed ? make_arguments_guarded(function_, memory_, guard_) : make_arguments(function_, memory_)) : TSUCHI_NOT_FOUND;
}

EngineCall::~EngineCall() {
    if (memory_) destroy_arguments(function_, memory_);
}

TSUCHI_Result EngineCall::set(const char* name, const Value& value) {
    if (state_ != TSUCHI_OK) return state_;
    if (!guard_.permits()) return TSUCHI_NOT_READY;
    Place place;
    const TSUCHI_Result found = find_place(function_, memory_, name, place);
    if (!guard_.permits()) return TSUCHI_NOT_READY;
    return found == TSUCHI_OK ? (guard_.allowed ? write_argument_guarded(place, value, guard_) : write_value(place, value)) : found;
}

TSUCHI_Result EngineCall::set_text(const char* name, const char* text) {
    if (guard_.allowed) return TSUCHI_UNSUPPORTED;
    if (state_ != TSUCHI_OK) return state_;
    Place place;
    const TSUCHI_Result found = find_place(function_, memory_, name, place);
    return found == TSUCHI_OK ? write_text(place, text) : found;
}

TSUCHI_Result EngineCall::invoke() {
    if (state_ != TSUCHI_OK) return state_;
    return guard_.allowed ? call_guarded(from_handle(object_), function_, memory_, guard_) : call(from_handle(object_), function_, memory_); // an object that is gone is not found
}

TSUCHI_Result EngineCall::invoke_before_play() {
    if (state_ != TSUCHI_OK) return state_;
    if (guard_.allowed) return TSUCHI_UNSUPPORTED;
    return call_object_dispatch(from_handle(object_), function_, memory_);
}

TSUCHI_Result EngineCall::get(const char* name, Value& out) {
    if (state_ != TSUCHI_OK) return state_;
    if (!guard_.permits()) return TSUCHI_NOT_READY;
    Place place;
    const TSUCHI_Result found = find_place(function_, memory_, name, place);
    if (!guard_.permits()) return TSUCHI_NOT_READY;
    return found == TSUCHI_OK ? read_value(place, out) : found;
}

TSUCHI_Result EngineCall::objects(const char* name, uint32_t& count) {
    count = 0;
    Value value;
    const TSUCHI_Result read = get(name, value);
    if (read != TSUCHI_OK) return read;
    if (value.kind != ValueKind::array) return TSUCHI_INVALID;
    count = value.count;
    return TSUCHI_OK;
}

uint64_t EngineCall::object_at(const char* name, uint32_t index) {
    // The element's path: the name, then the index in brackets.
    char path[64], digits[10];
    const size_t length = strlen(name);
    uint32_t count = 0;
    do {
        digits[count++] = char('0' + index % 10);
        index /= 10;
    } while (index);
    if (length + count + 3 > sizeof path) return 0;
    memcpy(path, name, length);
    size_t at = length;
    path[at++] = '[';
    while (count) path[at++] = digits[--count];
    path[at++] = ']';
    path[at] = 0;
    Value value;
    return get(path, value) == TSUCHI_OK && value.kind == ValueKind::object ? from_handle(value.object) : 0;
}

} // namespace tsuchi::game
