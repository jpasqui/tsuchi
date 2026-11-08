// One call of an engine function by name, for the framework's own use: its
// arguments are set by name, it is invoked, and what it gives back is read by
// name. The memory the arguments live in goes back to the engine when the call
// goes out of scope.
#pragma once
#include <stdint.h>

#include "reflect.h"

namespace tsuchi::game {

class EngineCall {
public:
    // A function of the object's class or of one of its parents.
    EngineCall(uint64_t object, const char* function);
    EngineCall(uint64_t object, const char* function, CallGuard guard);
    ~EngineCall();
    EngineCall(const EngineCall&) = delete;
    EngineCall& operator=(const EngineCall&) = delete;

    // TSUCHI_OK when the function was found and has memory for its arguments.
    TSUCHI_Result ready() const { return state_; }
    TSUCHI_Result set(const char* name, const Value& value);
    // A name or a string argument, from UTF-8.
    TSUCHI_Result set_text(const char* name, const char* text);
    TSUCHI_Result invoke();
    // Through the dispatch every object has (call_object_dispatch), for a call that has to run before play.
    TSUCHI_Result invoke_before_play();
    TSUCHI_Result get(const char* name, Value& out);
    // An array of objects the function gave back: how many, and each one's address.
    // TSUCHI_INVALID when the name is not an array that can be read.
    TSUCHI_Result objects(const char* name, uint32_t& count);
    uint64_t object_at(const char* name, uint32_t index);

private:
    uint64_t object_, function_ = 0, memory_ = 0;
    TSUCHI_Result state_;
    CallGuard guard_;
};

inline Value boolean_value(bool value) {
    Value out;
    out.kind = ValueKind::boolean;
    out.boolean = value;
    return out;
}
inline Value integer_value(int64_t value) {
    Value out;
    out.kind = ValueKind::integer;
    out.integer = value;
    return out;
}
inline Value number_value(double value) {
    Value out;
    out.kind = ValueKind::number;
    out.number = value;
    return out;
}
inline Value object_value(uint64_t handle) {
    Value out;
    out.kind = ValueKind::object;
    out.object = handle;
    return out;
}

} // namespace tsuchi::game
